# Testudo — AI-Powered Semantic Search Engine for Intelligent File Retrieval

A search engine that finds files by meaning, not exact keywords. Searching "car" finds a document that only says "automobile," because both get compared as vectors, not as text strings.

Built as a combined OS + DBMS + AI/NLP project (Team T312), the backend genuinely implements disk-level storage simulation and a full relational database alongside the semantic search itself, rather than treating those as separate slides in a report.

## What it actually does

Upload a document, and the system:
1. Splits it into overlapping chunks
2. Embeds each chunk into a vector using Ollama's `nomic-embed-text`
3. Writes the chunk's raw text across simulated disk blocks, chained together FAT-style
4. Stores the metadata, folder, owner, and embedding in a relational database
5. Grants the uploader access through a permissions table

Ask a question, and it:
1. Embeds the question the same way
2. Finds the nearest stored chunks (HNSW, KD-Tree, or brute force — your choice)
3. Filters results by the requesting user's actual permissions
4. Hands the surviving chunks to `llama3.2` to write an answer
5. Logs the exchange, so the conversation survives a page refresh or a server restart

## OS concepts, actually implemented

- **Disk blocks + FAT allocation** — files are split across fixed-size blocks, each pointing to the next, so a file can be scattered across the disk and still reconstruct correctly
- **All six standard disk scheduling algorithms** — FCFS, SSTF, SCAN, C-SCAN, LOOK, C-LOOK, comparable side by side against the same pending request queue
- **Directories with nesting** — `os_notes/scheduling` creates a real parent-child folder relationship
- **Access control** — owner vs. read-only, enforced at the code level (read-only genuinely cannot delete, not just labeled)
- **Mutual exclusion** — a single mutex protects all shared state across concurrent requests, deliberately coarse-grained rather than several fine-grained locks, to avoid deadlock by construction
- **CPU scheduling demo** — FCFS, SJF, Priority, and Round Robin, run against a fixed example process set

## DBMS concepts, actually implemented

- A 9-table relational schema (users, directories, documents, embeddings, permissions, query logs, and their join tables)
- Explicit indexes on the columns actually queried, not just default primary-key indexes
- Real transactions — a document's metadata and embedding are written atomically
- A named SQL **view** (`document_summary`) instead of a repeated join string
- A **trigger** that logs every document deletion automatically, with no application code writing to the audit table directly
- Live joined queries visible through a DBMS Explorer in the UI

## AI/NLP concepts, actually implemented

- Text embedding via Ollama (`nomic-embed-text`)
- Three search algorithms — brute force, KD-Tree, and a from-scratch HNSW implementation
- Retrieval-augmented generation via `llama3.2`, with retrieved context filtered by access control before it ever reaches the model

## Running it

See [`HOW_TO_RUN.md`](./HOW_TO_RUN.md) for full setup. Short version:

```bash
# Install Ollama models
ollama pull nomic-embed-text
ollama pull llama3.2

# Build with CMake
cmake -B build
cmake --build build

# Run (from the folder holding index.html)
./build/semantic_search
```

Prefer a one-line compile? That still works:

```bash
g++ -std=c++17 -O2 -I src -I . src/main.cpp -o db -lsqlite3 -lpthread   # add -lws2_32 on Windows
./db
```

Then open `index.html` in a browser. The backend listens on `localhost:8080`.

## Project structure

The backend is split into one module per layer, matching the original design proposal:

```
src/
  main.cpp              — entry point + REST API routing only
  os/
    virtual_disk.h      — fixed-size blocks + FAT-style chaining
    disk_scheduler.h    — FCFS, SSTF, SCAN, C-SCAN, LOOK, C-LOOK
    cpu_scheduler.h     — FCFS, SJF, Priority, Round Robin (demo)
  dbms/
    dbms_layer.h        — SQLite schema, indexes, view, trigger, ACL
  ai/
    distance.h          — vector type + cosine/euclidean/manhattan
    brute_force.h       — exhaustive search
    kd_tree.h           — k-d tree index
    hnsw.h              — HNSW graph index
    chunker.h           — overlapping text chunker
    ollama_client.h     — local Ollama embed/generate client
    vector_db.h         — demo 16-D index (Search Vectors tab)
    document_db.h       — real document index over Ollama embeddings
  api/
    json_util.h         — JSON helpers + CORS
    demo_data.h         — seed vectors for the demo tab
CMakeLists.txt          — cross-platform build
httplib.h               — third-party HTTP library (cpp-httplib)
index.html              — frontend, single self-contained file
nlp_engine/             — standalone Python embedding script (all-MiniLM-L6-v2, not wired into the live pipeline)
accuracy_comparison.py  — compares TF-IDF against the real semantic search
```

The earlier single-file version (`main.cpp` + `storage.h` at the root) has been split into the tree above. Each header owns one concern, and `main.cpp` now holds only routing and the restart-recovery sequence.

## Benchmarks

Search timing on the same query at the current scale (~20–30 vectors):

| Index | Time |
|---|---|
| KD-tree | 21 µs |
| Brute force | 37 µs |
| HNSW | 118 µs |

HNSW is slowest here on purpose, not by accident — its graph-traversal overhead only pays off once the dataset is large enough that a full scan becomes the expensive option. At this scale, scanning a handful of vectors directly wins.

## Known limitations

- **Scalability.** The vector index lives entirely in memory and is rebuilt on every restart, so the whole dataset must fit in RAM. This is fine at academic scale but doesn't match a production vector database (FAISS, Chroma) that keeps the index on disk. The fix is a disk-backed, shardable index.
- **Ask AI is slow on CPU.** The `llama3.2` answer generation is heavy and runs fast only with a GPU; on a CPU-only machine it's slow. Embedding and search themselves stay fast. A smaller model (`llama3.2:1b`) helps.
- **Deletion is disk-bound.** Each delete rewrites the whole virtual-disk file and commits SQLite, so it pauses on slow drives.
- **No login or encryption.** Accounts are plain usernames added through the System tab; there's no password auth, and data isn't encrypted at rest.
- **No content-editing endpoint** — create and delete only.
- **LOOK/C-LOOK** produce the same output as SCAN/C-SCAN in this simulation, since the model doesn't track the disk's physical boundary as a distinct stop.
- **SQLite has no stored procedures** — the equivalent logic lives in `DBMSLayer`'s C++ methods instead.

Full details in [`FUTURE_RESEARCH.md`](./FUTURE_RESEARCH.md) and [`DOCUMENTATION.md`](./DOCUMENTATION.md).

## Tech stack

C++17, SQLite, cpp-httplib, Ollama (`nomic-embed-text`, `llama3.2`), vanilla HTML/CSS/JS on the frontend. Cross-platform build via CMake.

## References

- Malkov & Yashunin — *Efficient and robust approximate nearest neighbor search using HNSW graphs* (arXiv:1603.09320)
- Reimers & Gurevych — *Sentence-BERT: Sentence Embeddings using Siamese BERT-Networks* (arXiv:1908.10084)
- Nussbaum et al. — *Nomic Embed: Training a Reproducible Long Context Text Embedder* (arXiv:2402.01613)
- Silberschatz, Galvin, Gagne — *Operating System Concepts*
- Elmasri, Navathe — *Fundamentals of Database Systems*
- Ollama documentation, ollama.com · SQLite documentation, sqlite.org
