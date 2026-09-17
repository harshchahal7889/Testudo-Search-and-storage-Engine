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

# Compile
g++ -std=c++17 -O2 main.cpp -o db -lws2_32 -lsqlite3

# Run
./db
```

Then open `index.html` in a browser. The backend listens on `localhost:8080`.

## Project structure

```
main.cpp        — REST API, request handling, all endpoint logic
storage.h       — VirtualDisk, DiskScheduler, CpuScheduler, DBMSLayer
httplib.h       — third-party HTTP library (cpp-httplib)
index.html      — frontend, single self-contained file
nlp_engine/     — standalone Python embedding script (all-MiniLM-L6-v2, not currently wired into the live pipeline)
accuracy_comparison.py — compares TF-IDF against the real semantic search
```

A module split into `os_module/`, `database/`, `interface/` matching the original design proposal is planned but not yet done — see [`IMPLEMENTATION_PLAN.md`](./IMPLEMENTATION_PLAN.md).

## Known limitations

- Two demo user accounts by default (`harsh`, `guest`); more can be added through the System tab, but there's no real login system
- No content-editing endpoint — only create and delete
- LOOK and C-LOOK produce the same output as SCAN and C-SCAN in the current simulation, since the model doesn't track the disk's physical boundary as a distinct stop
- SQLite has no stored procedures; the equivalent logic lives in `DBMSLayer`'s C++ methods instead

Full details in [`FUTURE_RESEARCH.md`](./FUTURE_RESEARCH.md) and [`DOCUMENTATION.md`](./DOCUMENTATION.md).

## Tech stack

C++17, SQLite, cpp-httplib, Ollama (`nomic-embed-text`, `llama3.2`), vanilla HTML/CSS/JS on the frontend.

## References

- Silberschatz, Galvin, Gagne — *Operating System Concepts*
- Elmasri, Navathe — *Fundamentals of Database Systems*
- Sentence Transformers documentation, sbert.net
- Ollama documentation, ollama.com
