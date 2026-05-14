# rpull

A small C CLI that recursively finds Git repositories under a directory and runs `git pull` in each one.

## Build

```bash
make
```

## Install

```bash
make install
```

That installs the binary to `~/.local/bin/rpull`.

Make sure `~/.local/bin` is on your `PATH`.

## Use

```bash
rpull
rpull ~
rpull ~/projects -j 8
rpull --dry-run
```

## Behavior

- Scans the target directory recursively
- Detects Git repositories by the presence of a `.git` directory or file
- Runs pulls in parallel with a worker pool
- Skips `.git` internals while scanning
- Prints a summary of successes and failures

## Notes

- The default root is the current directory
- `git pull` output is shown directly in the terminal
- If any repository fails, the program exits with a non-zero status
