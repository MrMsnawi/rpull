#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} RepoList;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} DirStack;

typedef struct {
    const RepoList *repos;
    size_t next_index;
    size_t success_count;
    size_t failure_count;
    bool dry_run;
    pthread_mutex_t lock;
    pthread_mutex_t print_lock;
} WorkQueue;

static void repo_list_free(RepoList *list) {
    if (!list) {
        return;
    }

    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void stack_free(DirStack *stack) {
    if (!stack) {
        return;
    }

    for (size_t i = 0; i < stack->count; i++) {
        free(stack->items[i]);
    }
    free(stack->items);
    stack->items = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

static int repo_list_push(RepoList *list, const char *path) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 32 : list->capacity * 2;
        char **new_items = realloc(list->items, new_capacity * sizeof(*new_items));
        if (!new_items) {
            return -1;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count] = strdup(path);
    if (!list->items[list->count]) {
        return -1;
    }

    list->count++;
    return 0;
}

static int stack_push(DirStack *stack, const char *path) {
    if (stack->count == stack->capacity) {
        size_t new_capacity = stack->capacity == 0 ? 64 : stack->capacity * 2;
        char **new_items = realloc(stack->items, new_capacity * sizeof(*new_items));
        if (!new_items) {
            return -1;
        }
        stack->items = new_items;
        stack->capacity = new_capacity;
    }

    stack->items[stack->count] = strdup(path);
    if (!stack->items[stack->count]) {
        return -1;
    }

    stack->count++;
    return 0;
}

static char *stack_pop(DirStack *stack) {
    if (stack->count == 0) {
        return NULL;
    }

    return stack->items[--stack->count];
}

static bool has_git_marker(const char *dir) {
    char git_path[PATH_MAX];
    int written = snprintf(git_path, sizeof(git_path), "%s/.git", dir);
    if (written < 0 || (size_t)written >= sizeof(git_path)) {
        return false;
    }

    struct stat st;
    return stat(git_path, &st) == 0;
}

static int scan_repositories(const char *root, RepoList *repos) {
    DirStack stack = {0};

    if (stack_push(&stack, root) != 0) {
        stack_free(&stack);
        return -1;
    }

    while (stack.count > 0) {
        char *current = stack_pop(&stack);
        if (!current) {
            break;
        }

        if (has_git_marker(current)) {
            if (repo_list_push(repos, current) != 0) {
                free(current);
                stack_free(&stack);
                return -1;
            }
        }

        DIR *dir = opendir(current);
        if (!dir) {
            free(current);
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || strcmp(entry->d_name, ".git") == 0) {
                continue;
            }

            char child_path[PATH_MAX];
            int written = snprintf(child_path, sizeof(child_path), "%s/%s", current, entry->d_name);
            if (written < 0 || (size_t)written >= sizeof(child_path)) {
                continue;
            }

            struct stat st;
            if (lstat(child_path, &st) != 0) {
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                if (stack_push(&stack, child_path) != 0) {
                    closedir(dir);
                    free(current);
                    stack_free(&stack);
                    return -1;
                }
            }
        }

        closedir(dir);
        free(current);
    }

    stack_free(&stack);
    return 0;
}

static int run_git_pull(const char *repo) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        execlp("git", "git", "-C", repo, "pull", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return -1;
}

static void *worker_main(void *arg) {
    WorkQueue *queue = arg;

    for (;;) {
        pthread_mutex_lock(&queue->lock);
        size_t index = queue->next_index;
        if (index >= queue->repos->count) {
            pthread_mutex_unlock(&queue->lock);
            break;
        }
        queue->next_index++;
        const char *repo = queue->repos->items[index];
        pthread_mutex_unlock(&queue->lock);

        pthread_mutex_lock(&queue->print_lock);
        if (queue->dry_run) {
            printf("[DRY]  %s\n", repo);
        } else {
            printf("[RUN]  %s\n", repo);
        }
        fflush(stdout);
        pthread_mutex_unlock(&queue->print_lock);

        int rc = queue->dry_run ? 0 : run_git_pull(repo);

        pthread_mutex_lock(&queue->print_lock);
        if (rc == 0) {
            printf("[OK]   %s\n", repo);
        } else {
            printf("[FAIL] %s (exit %d)\n", repo, rc);
        }
        fflush(stdout);
        pthread_mutex_unlock(&queue->print_lock);

        pthread_mutex_lock(&queue->lock);
        if (rc == 0) {
            queue->success_count++;
        } else {
            queue->failure_count++;
        }
        pthread_mutex_unlock(&queue->lock);
    }

    return NULL;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options] [root]\n\n"
            "Options:\n"
            "  -j, --jobs N        Number of parallel git pulls (default: CPU count or 4)\n"
            "  -n, --dry-run       Discover repositories without running git pull\n"
            "  -h, --help          Show this help message\n",
            prog);
}

int main(int argc, char **argv) {
    int jobs = 0;
    bool dry_run = false;

    static const struct option long_options[] = {
        {"jobs", required_argument, NULL, 'j'},
        {"dry-run", no_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "j:nh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'j':
                jobs = atoi(optarg);
                break;
            case 'n':
                dry_run = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 2;
        }
    }

    const char *root_arg = ".";
    if (optind < argc) {
        root_arg = argv[optind++];
    }
    if (optind < argc) {
        fprintf(stderr, "error: too many arguments\n");
        print_usage(argv[0]);
        return 2;
    }

    char root[PATH_MAX];
    if (!realpath(root_arg, root)) {
        fprintf(stderr, "error: cannot resolve path '%s': %s\n", root_arg, strerror(errno));
        return 2;
    }

    if (jobs <= 0) {
        long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
        jobs = (cpu_count > 0) ? (int)cpu_count : 4;
        if (jobs > 8) {
            jobs = 8;
        }
    }
    if (jobs < 1) {
        jobs = 1;
    }

    RepoList repos = {0};
    if (scan_repositories(root, &repos) != 0) {
        fprintf(stderr, "error: failed to scan repositories\n");
        repo_list_free(&repos);
        return 1;
    }

    if (repos.count == 0) {
        printf("No git repositories found under %s\n", root);
        repo_list_free(&repos);
        return 0;
    }

    printf("Found %zu repositories under %s\n", repos.count, root);

    WorkQueue queue = {
        .repos = &repos,
        .next_index = 0,
        .success_count = 0,
        .failure_count = 0,
        .dry_run = dry_run,
    };

    if (pthread_mutex_init(&queue.lock, NULL) != 0 || pthread_mutex_init(&queue.print_lock, NULL) != 0) {
        fprintf(stderr, "error: failed to initialize mutexes\n");
        repo_list_free(&repos);
        return 1;
    }

    pthread_t *threads = calloc((size_t)jobs, sizeof(*threads));
    if (!threads) {
        fprintf(stderr, "error: out of memory\n");
        pthread_mutex_destroy(&queue.lock);
        pthread_mutex_destroy(&queue.print_lock);
        repo_list_free(&repos);
        return 1;
    }

    int created = 0;
    for (int i = 0; i < jobs; i++) {
        if (pthread_create(&threads[i], NULL, worker_main, &queue) != 0) {
            break;
        }
        created++;
    }

    for (int i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    pthread_mutex_destroy(&queue.lock);
    pthread_mutex_destroy(&queue.print_lock);

    printf("Done. Success: %zu, Failed: %zu, Total: %zu\n",
           queue.success_count,
           queue.failure_count,
           repos.count);

    repo_list_free(&repos);
    return queue.failure_count == 0 ? 0 : 1;
}
