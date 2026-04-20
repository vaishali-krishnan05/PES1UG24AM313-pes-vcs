# PES Version Control System (PES-VCS)

## Overview

This project is a simplified version control system inspired by Git. It implements core concepts such as content-addressable storage, trees, commits, and history traversal.

The system allows users to track file changes, create commits, and view commit history.

---

## Features

* Content-addressable object store using SHA-256
* File staging using an index
* Tree structure representing directory hierarchy
* Commit creation with parent linking
* Commit history traversal (log)
* Deduplication of identical objects
* Atomic updates for consistency

---

## Project Structure

* `object.c` → Handles storage and retrieval of objects
* `tree.c` → Builds directory tree from index
* `index.c` → Manages staging area
* `commit.c` → Creates commits and manages history
* `pes.c` → CLI interface for commands

---

## Build Instructions

```bash
make clean
make
```

---

## Usage

### Initialize repository

```bash
./pes init
```

### Add files to staging

```bash
./pes add <file>
```

### Commit changes

```bash
./pes commit -m "message"
```

### View commit history

```bash
./pes log
```

---

## Example Workflow

```bash
./pes init

echo "Hello" > hello.txt
./pes add hello.txt
./pes commit -m "Initial commit"

echo "World" >> hello.txt
./pes add hello.txt
./pes commit -m "Add world"

./pes log
```

---

## Object Storage

Objects are stored in:

```
.pes/objects/XX/YYYY...
```

* `XX` → First two characters of SHA-256 hash
* Remaining hash → filename

Object format:

```
<type> <size>\0<data>
```

Types:

* blob → file content
* tree → directory structure
* commit → snapshot metadata

---

## Commit Structure

Each commit contains:

* Tree hash (snapshot of files)
* Parent commit (if any)
* Author and timestamp
* Commit message

This forms a linked structure similar to Git.

---

## Key Concepts

* **Content Addressing**: Files are stored based on hash, not name
* **Deduplication**: Same content is stored only once
* **Immutability**: Objects never change once written
* **Atomic Updates**: Ensures repository consistency
* **Linked Commits**: Each commit points to its parent

---

## Notes

* `.pes/` directory stores all repository data
* It should NOT be committed to GitHub
* Test files (`*.txt`, `.bak`) are excluded using `.gitignore`

---

## Author

Vaishali Krishnan

---

## Conclusion

This project demonstrates fundamental version control concepts such as hashing, snapshots, and history tracking, providing a simplified understanding of how systems like Git work internally.
