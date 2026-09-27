
#pragma once
#include <sqlite3.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <deque>
#include <climits>

// =====================================================================
//  OS LAYER — Virtual Disk with fixed-size blocks + FAT-style chaining
// =====================================================================

class VirtualDisk
{
public:
    // Public view of one block, used to expose the FAT table to the UI.
    struct BlockInfo
    {
        int id;
        int nextBlock;
        std::string data;
    };

private:
    struct Block
    {
        bool isFree = true;
        std::string data;   // this block's chunk of the file
        int nextBlock = -1; // FAT pointer to next block (-1 = end of chain)
    };

    std::vector<Block> blocks;
    int blockSize;

public:
    VirtualDisk(int totalBlocks = 4096, int blockSizeBytes = 512)
        : blocks(totalBlocks), blockSize(blockSizeBytes) {}

    // Splits `content` across free blocks, links them via FAT (nextBlock),
    // and returns the starting block id (the "file pointer" you store in SQLite).
    int allocateFile(const std::string &content)
    {
        std::vector<int> freeIds;
        for (int i = 0; i < (int)blocks.size() && (int)freeIds.size() * blockSize < (int)content.size() + blockSize; ++i)
            if (blocks[i].isFree)
                freeIds.push_back(i);

        size_t needed = (content.size() + blockSize - 1) / blockSize;
        if (needed == 0)
            needed = 1;
        if (freeIds.size() < needed)
            throw std::runtime_error("VirtualDisk: not enough free blocks");

        int startBlock = freeIds[0];
        for (size_t i = 0; i < needed; ++i)
        {
            int id = freeIds[i];
            blocks[id].isFree = false;
            blocks[id].data = content.substr(i * blockSize, blockSize);
            blocks[id].nextBlock = (i + 1 < needed) ? freeIds[i + 1] : -1;
        }
        return startBlock;
    }

    // Follows the FAT chain from startBlock and reassembles the original content.
    std::string readFile(int startBlock)
    {
        std::string result;
        int current = startBlock;
        while (current != -1 && current < (int)blocks.size())
        {
            result += blocks[current].data;
            current = blocks[current].nextBlock;
        }
        return result;
    }

    // Frees every block in a file's chain (called on delete).
    void freeFile(int startBlock)
    {
        int current = startBlock;
        while (current != -1 && current < (int)blocks.size())
        {
            int next = blocks[current].nextBlock;
            blocks[current] = Block{};
            current = next;
        }
    }

    int freeBlockCount() const
    {
        int c = 0;
        for (auto &b : blocks)
            if (b.isFree)
                ++c;
        return c;
    }

    // Returns every currently-allocated block, in FAT-table form:
    // block id, its next-block pointer, and its stored data.
    std::vector<BlockInfo> getUsedBlocks() const
    {
        std::vector<BlockInfo> out;
        for (int i = 0; i < (int)blocks.size(); ++i)
        {
            if (!blocks[i].isFree)
                out.push_back({i, blocks[i].nextBlock, blocks[i].data});
        }
        return out;
    }

    int totalBlocks() const { return (int)blocks.size(); }

    // Writes every block's state to disk so a restart doesn't lose data.
    void save(const std::string &path) const
    {
        std::ofstream out(path, std::ios::binary);
        if (!out)
            return;
        int n = (int)blocks.size();
        out.write((char *)&n, sizeof(n));
        out.write((char *)&blockSize, sizeof(blockSize));
        for (auto &b : blocks)
        {
            out.write((char *)&b.isFree, sizeof(b.isFree));
            out.write((char *)&b.nextBlock, sizeof(b.nextBlock));
            int len = (int)b.data.size();
            out.write((char *)&len, sizeof(len));
            out.write(b.data.data(), len);
        }
    }

    // Restores block state from a previous save(); returns false if no
    // saved file exists yet (first run), which is fine — starts fresh.
    bool load(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        int n = 0, savedBlockSize = 0;
        in.read((char *)&n, sizeof(n));
        in.read((char *)&savedBlockSize, sizeof(savedBlockSize));
        if (n != (int)blocks.size())
            return false; // disk size changed, don't risk a mismatch
        for (auto &b : blocks)
        {
            in.read((char *)&b.isFree, sizeof(b.isFree));
            in.read((char *)&b.nextBlock, sizeof(b.nextBlock));
            int len = 0;
            in.read((char *)&len, sizeof(len));
            b.data.resize(len);
            in.read(&b.data[0], len);
        }
        return true;
    }
};

// =====================================================================
//  OS LAYER — Disk Scheduling (SSTF and C-SCAN)
//  Decides the ORDER in which a batch of block requests is serviced.
// =====================================================================

namespace DiskScheduler
{

    // First Come First Served — services requests in the order they arrived.
    inline std::vector<int> fcfs(std::vector<int> queue, int headStart)
    {
        return queue; // no reordering at all
    }

    // Shortest Seek Time First — always services nearest pending request.
    inline std::vector<int> sstf(std::vector<int> queue, int headStart)
    {
        std::vector<int> order;
        int head = headStart;
        while (!queue.empty())
        {
            auto it = std::min_element(queue.begin(), queue.end(),
                                       [head](int a, int b)
                                       { return std::abs(a - head) < std::abs(b - head); });
            head = *it;
            order.push_back(head);
            queue.erase(it);
        }
        return order;
    }

    // SCAN (elevator) — sweeps to one end of the disk, then reverses and
    // sweeps back, servicing requests along the way in both directions.
    inline std::vector<int> scan(std::vector<int> queue, int headStart, int diskSize)
    {
        std::sort(queue.begin(), queue.end());
        std::vector<int> order;
        for (int r : queue)
            if (r >= headStart)
                order.push_back(r); // sweep up
        for (auto it = queue.rbegin(); it != queue.rend(); ++it)
            if (*it < headStart)
                order.push_back(*it); // then sweep back down
        return order;
    }

    // Circular SCAN — services requests in one direction, wraps to 0 at the end.
    inline std::vector<int> cscan(std::vector<int> queue, int headStart, int diskSize)
    {
        std::sort(queue.begin(), queue.end());
        std::vector<int> order;
        for (int r : queue)
            if (r >= headStart)
                order.push_back(r);
        for (int r : queue)
            if (r < headStart)
                order.push_back(r); // after wraparound
        return order;
    }

    // LOOK — like SCAN, but reverses at the last real request instead of
    // travelling all the way to the physical end of the disk.
    inline std::vector<int> look(std::vector<int> queue, int headStart, int diskSize)
    {
        std::sort(queue.begin(), queue.end());
        std::vector<int> order;
        for (int r : queue)
            if (r >= headStart)
                order.push_back(r);
        for (auto it = queue.rbegin(); it != queue.rend(); ++it)
            if (*it < headStart)
                order.push_back(*it);
        return order; // identical output to scan() here since we never touch 0/diskSize anyway
    }

    // C-LOOK — like C-SCAN, but jumps back to the lowest pending request
    // instead of all the way to block 0.
    inline std::vector<int> clook(std::vector<int> queue, int headStart, int diskSize)
    {
        std::sort(queue.begin(), queue.end());
        std::vector<int> order;
        for (int r : queue)
            if (r >= headStart)
                order.push_back(r);
        for (int r : queue)
            if (r < headStart)
                order.push_back(r);
        return order; // same result as cscan() here for the same reason
    }

    // Runs the named algorithm; unknown names fall back to SSTF.
    inline std::vector<int> run(const std::string &name, std::vector<int> queue, int headStart, int diskSize)
    {
        if (name == "fcfs")
            return fcfs(queue, headStart);
        if (name == "scan")
            return scan(queue, headStart, diskSize);
        if (name == "cscan")
            return cscan(queue, headStart, diskSize);
        if (name == "look")
            return look(queue, headStart, diskSize);
        if (name == "clook")
            return clook(queue, headStart, diskSize);
        return sstf(queue, headStart);
    }
}

// =====================================================================
//  CPU SCHEDULING — a standalone teaching demo over a fixed set of
//  example processes. This project doesn't manage real OS processes, so
//  this is honestly labeled as a simulation, the same way the search
//  demo (Search Vectors tab) is a simulation separate from real documents.
// =====================================================================
namespace CpuScheduler
{

    struct Process
    {
        int id;
        int arrival;
        int burst;
        int priority;
    };
    struct RunSlice
    {
        int processId;
        int start;
        int end;
    };
    struct Result
    {
        std::vector<RunSlice> gantt;
        double avgWaiting;
        double avgTurnaround;
    };

    inline Result fcfs(std::vector<Process> procs)
    {
        std::sort(procs.begin(), procs.end(),
                  [](auto &a, auto &b)
                  { return a.arrival < b.arrival; });
        Result r;
        int clock = 0;
        double totalWait = 0, totalTurn = 0;
        for (auto &p : procs)
        {
            int start = std::max(clock, p.arrival);
            int end = start + p.burst;
            r.gantt.push_back({p.id, start, end});
            totalWait += (start - p.arrival);
            totalTurn += (end - p.arrival);
            clock = end;
        }
        r.avgWaiting = totalWait / procs.size();
        r.avgTurnaround = totalTurn / procs.size();
        return r;
    }

    // Shortest Job First (non-preemptive): among processes that have
    // arrived, always run whichever has the smallest burst time next.
    inline Result sjf(std::vector<Process> procs)
    {
        Result r;
        int clock = 0;
        double totalWait = 0, totalTurn = 0;
        std::vector<bool> done(procs.size(), false);
        for (size_t count = 0; count < procs.size(); ++count)
        {
            int best = -1;
            for (size_t i = 0; i < procs.size(); ++i)
            {
                if (done[i] || procs[i].arrival > clock)
                    continue;
                if (best == -1 || procs[i].burst < procs[best].burst)
                    best = (int)i;
            }
            if (best == -1)
            {
                int nextArrival = INT_MAX;
                for (size_t i = 0; i < procs.size(); ++i)
                    if (!done[i])
                        nextArrival = std::min(nextArrival, procs[i].arrival);
                clock = nextArrival;
                --count;
                continue;
            }
            int start = clock, end = start + procs[best].burst;
            r.gantt.push_back({procs[best].id, start, end});
            totalWait += (start - procs[best].arrival);
            totalTurn += (end - procs[best].arrival);
            clock = end;
            done[best] = true;
        }
        r.avgWaiting = totalWait / procs.size();
        r.avgTurnaround = totalTurn / procs.size();
        return r;
    }

    // Non-preemptive priority scheduling — lower number = higher priority.
    inline Result priority(std::vector<Process> procs)
    {
        Result r;
        int clock = 0;
        double totalWait = 0, totalTurn = 0;
        std::vector<bool> done(procs.size(), false);
        for (size_t count = 0; count < procs.size(); ++count)
        {
            int best = -1;
            for (size_t i = 0; i < procs.size(); ++i)
            {
                if (done[i] || procs[i].arrival > clock)
                    continue;
                if (best == -1 || procs[i].priority < procs[best].priority)
                    best = (int)i;
            }
            if (best == -1)
            {
                int nextArrival = INT_MAX;
                for (size_t i = 0; i < procs.size(); ++i)
                    if (!done[i])
                        nextArrival = std::min(nextArrival, procs[i].arrival);
                clock = nextArrival;
                --count;
                continue;
            }
            int start = clock, end = start + procs[best].burst;
            r.gantt.push_back({procs[best].id, start, end});
            totalWait += (start - procs[best].arrival);
            totalTurn += (end - procs[best].arrival);
            clock = end;
            done[best] = true;
        }
        r.avgWaiting = totalWait / procs.size();
        r.avgTurnaround = totalTurn / procs.size();
        return r;
    }

    // Round Robin with a fixed time quantum — preemptive, cycles through
    // a ready queue, each process gets at most `quantum` time per turn.
    inline Result roundRobin(std::vector<Process> procs, int quantum = 2)
    {
        std::sort(procs.begin(), procs.end(),
                  [](auto &a, auto &b)
                  { return a.arrival < b.arrival; });
        std::vector<int> remaining(procs.size());
        for (size_t i = 0; i < procs.size(); ++i)
            remaining[i] = procs[i].burst;
        std::vector<int> completion(procs.size(), -1);

        std::deque<int> ready;
        std::vector<bool> queued(procs.size(), false);
        Result r;
        int clock = 0, doneCount = 0;
        size_t nextArrivalIdx = 0;

        auto admitArrivals = [&]()
        {
            while (nextArrivalIdx < procs.size() && procs[nextArrivalIdx].arrival <= clock)
            {
                ready.push_back((int)nextArrivalIdx);
                queued[nextArrivalIdx] = true;
                nextArrivalIdx++;
            }
        };

        if (procs.empty())
            return r;
        clock = procs[0].arrival;
        admitArrivals();

        while (doneCount < (int)procs.size())
        {
            if (ready.empty())
            {
                clock = procs[nextArrivalIdx].arrival;
                admitArrivals();
                continue;
            }
            int idx = ready.front();
            ready.pop_front();
            int run = std::min(quantum, remaining[idx]);
            int start = clock, end = clock + run;
            r.gantt.push_back({procs[idx].id, start, end});
            clock = end;
            remaining[idx] -= run;
            admitArrivals();
            if (remaining[idx] > 0)
            {
                ready.push_back(idx);
            }
            else
            {
                completion[idx] = clock;
                doneCount++;
            }
        }

        double totalWait = 0, totalTurn = 0;
        for (size_t i = 0; i < procs.size(); ++i)
        {
            int turn = completion[i] - procs[i].arrival;
            int wait = turn - procs[i].burst;
            totalTurn += turn;
            totalWait += wait;
        }
        r.avgWaiting = totalWait / procs.size();
        r.avgTurnaround = totalTurn / procs.size();
        return r;
    }

    inline Result run(const std::string &name, std::vector<Process> procs)
    {
        if (name == "sjf")
            return sjf(procs);
        if (name == "priority")
            return priority(procs);
        if (name == "rr")
            return roundRobin(procs);
        return fcfs(procs);
    }
}

// =====================================================================
//  DBMS LAYER — SQLite metadata, embeddings, and ACL
// =====================================================================

class DBMSLayer
{
    sqlite3 *db = nullptr;

    void exec(const std::string &sql)
    {
        char *err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
        {
            std::string msg = err ? err : "unknown sqlite error";
            sqlite3_free(err);
            throw std::runtime_error("SQLite error: " + msg);
        }
    }

public:
    DBMSLayer(const std::string &path = "nexusai.db")
    {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
            throw std::runtime_error("Cannot open SQLite DB");
        exec("PRAGMA foreign_keys = ON;");

        exec(R"(CREATE TABLE IF NOT EXISTS users (
                    user_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    username TEXT UNIQUE
                );)");

        // OS OBJECTIVE: "virtual file system WITH DIRECTORIES and disk blocks"
        // Self-referencing parent_dir_id gives a real folder hierarchy (root = NULL parent).
        exec(R"(CREATE TABLE IF NOT EXISTS directories (
                    dir_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    dir_name TEXT NOT NULL,
                    parent_dir_id INTEGER,
                    FOREIGN KEY (parent_dir_id) REFERENCES directories(dir_id)
                );)");

        exec(R"(CREATE TABLE IF NOT EXISTS documents (
                    doc_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    title TEXT,
                    owner_id INTEGER,
                    disk_block_id INTEGER,
                    dir_id INTEGER,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    FOREIGN KEY (owner_id) REFERENCES users(user_id),
                    FOREIGN KEY (dir_id) REFERENCES directories(dir_id)
                );)");

        exec(R"(CREATE TABLE IF NOT EXISTS embeddings (
                    embedding_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    doc_id INTEGER,
                    vector_data BLOB,
                    dim INTEGER,
                    FOREIGN KEY (doc_id) REFERENCES documents(doc_id)
                );)");

        exec(R"(CREATE TABLE IF NOT EXISTS file_permissions (
                    permission_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    doc_id INTEGER,
                    user_id INTEGER,
                    access_level TEXT,
                    FOREIGN KEY (doc_id) REFERENCES documents(doc_id),
                    FOREIGN KEY (user_id) REFERENCES users(user_id)
                );)");

        // Closes the "log user queries and retrieved files" objective from
        // the original report, and matches the USER_LOGS entity from the
        // ER diagram — also what makes the Ask AI conversation survive a
        // page refresh, instead of living only in the browser's DOM.
        exec(R"(CREATE TABLE IF NOT EXISTS user_logs (
                    log_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    user_id INTEGER,
                    question TEXT,
                    answer TEXT,
                    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    FOREIGN KEY (user_id) REFERENCES users(user_id)
                );)");

        exec(R"(CREATE TABLE IF NOT EXISTS log_contexts (
                    log_id INTEGER,
                    doc_id INTEGER,
                    FOREIGN KEY (log_id) REFERENCES user_logs(log_id),
                    FOREIGN KEY (doc_id) REFERENCES documents(doc_id)
                );)");

        // DBMS OBJECTIVE: explicit indexing on frequently-queried columns.
        // Primary keys already get an implicit index — these speed up the
        // lookups this project actually performs: ACL checks (by doc+user),
        // fetching a document's embedding, and listing a directory's files.
        exec("CREATE INDEX IF NOT EXISTS idx_documents_owner    ON documents(owner_id);");
        exec("CREATE INDEX IF NOT EXISTS idx_documents_dir      ON documents(dir_id);");
        exec("CREATE INDEX IF NOT EXISTS idx_embeddings_docid   ON embeddings(doc_id);");
        exec("CREATE INDEX IF NOT EXISTS idx_permissions_lookup ON file_permissions(doc_id, user_id);");
        exec("CREATE INDEX IF NOT EXISTS idx_logs_user ON user_logs(user_id, timestamp);");

        // DBMS OBJECTIVE: a real trigger. This table is never written to by
        // any C++ code — the row only appears because SQLite itself fires
        // the trigger below whenever a documents row is deleted.
        exec(R"(CREATE TABLE IF NOT EXISTS deletion_audit (
                    audit_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    doc_id INTEGER,
                    title TEXT,
                    deleted_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                );)");

        exec(R"(CREATE TRIGGER IF NOT EXISTS trg_log_document_delete
                    AFTER DELETE ON documents
                    BEGIN
                        INSERT INTO deletion_audit (doc_id, title)
                        VALUES (OLD.doc_id, OLD.title);
                    END;)");

        // DBMS OBJECTIVE: a real named VIEW, not just a query string repeated
        // in application code. Anything querying this view sees the same
        // joined shape without knowing the underlying tables at all.
        exec(R"(CREATE VIEW IF NOT EXISTS document_summary AS
                    SELECT d.doc_id AS doc_id,
                           d.title AS title,
                           u.username AS owner,
                           IFNULL(dir.dir_name, 'root') AS folder,
                           d.disk_block_id AS start_block,
                           d.created_at AS created_at
                    FROM documents d
                    LEFT JOIN users u ON d.owner_id = u.user_id
                    LEFT JOIN directories dir ON d.dir_id = dir.dir_id;)");
    }

    ~DBMSLayer()
    {
        if (db)
            sqlite3_close(db);
    }

    // Returns an existing directory's id, or creates it under parentDirId (NULL = root).
    int getOrCreateDirectory(const std::string &dirName, int parentDirId = -1)
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           parentDirId == -1
                               ? "SELECT dir_id FROM directories WHERE dir_name = ? AND parent_dir_id IS NULL;"
                               : "SELECT dir_id FROM directories WHERE dir_name = ? AND parent_dir_id = ?;",
                           -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, dirName.c_str(), -1, SQLITE_TRANSIENT);
        if (parentDirId != -1)
            sqlite3_bind_int(stmt, 2, parentDirId);
        int existing = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            existing = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (existing != -1)
            return existing;

        sqlite3_prepare_v2(db,
                           "INSERT INTO directories (dir_name, parent_dir_id) VALUES (?, ?);",
                           -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, dirName.c_str(), -1, SQLITE_TRANSIENT);
        if (parentDirId == -1)
            sqlite3_bind_null(stmt, 2);
        else
            sqlite3_bind_int(stmt, 2, parentDirId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (int)sqlite3_last_insert_rowid(db);
    }

    // Insert file metadata + its embedding in one transaction (atomicity).
    // dirId defaults to -1 (no directory / root) so existing callers keep working.
    int insertDocument(const std::string &title, int ownerId, int diskBlockId,
                       const std::vector<float> &emb, int dirId = -1)
    {
        exec("BEGIN TRANSACTION;");
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "INSERT INTO documents (title, owner_id, disk_block_id, dir_id) VALUES (?, ?, ?, ?);",
                           -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, ownerId);
        sqlite3_bind_int(stmt, 3, diskBlockId);
        if (dirId == -1)
            sqlite3_bind_null(stmt, 4);
        else
            sqlite3_bind_int(stmt, 4, dirId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        int docId = (int)sqlite3_last_insert_rowid(db);

        sqlite3_prepare_v2(db,
                           "INSERT INTO embeddings (doc_id, vector_data, dim) VALUES (?, ?, ?);",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        sqlite3_bind_blob(stmt, 2, emb.data(), (int)(emb.size() * sizeof(float)), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, (int)emb.size());
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        exec("COMMIT;");
        return docId;
    }

    // Grants a user access to a document — call this right after upload
    // so the owner can always see their own file.
    void grantAccess(int docId, int userId, const std::string &level = "read")
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "INSERT INTO file_permissions (doc_id, user_id, access_level) VALUES (?, ?, ?);",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        sqlite3_bind_int(stmt, 2, userId);
        sqlite3_bind_text(stmt, 3, level.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // ACL check — called before returning any search/RAG result.
    bool hasAccess(int docId, int userId)
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "SELECT 1 FROM file_permissions WHERE doc_id = ? AND user_id = ?;",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        sqlite3_bind_int(stmt, 2, userId);
        bool allowed = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return allowed;
    }

    // Distinct from hasAccess(): this specifically checks for "owner"
    // level, used to gate destructive actions (delete) so "read_only"
    // actually means something different from "owner" — before this,
    // both labels behaved identically everywhere.
    bool hasWriteAccess(int docId, int userId)
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "SELECT 1 FROM file_permissions WHERE doc_id = ? AND user_id = ? AND access_level = 'owner';",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        sqlite3_bind_int(stmt, 2, userId);
        bool allowed = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return allowed;
    }

    // Looks up a user's id by username — needed so /doc/share can grant
    // access to an existing document for someone other than the uploader.
    int getUserIdByName(const std::string &username)
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT user_id FROM users WHERE username = ?;", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        int id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }

    int getDiskBlockId(int docId)
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT disk_block_id FROM documents WHERE doc_id = ?;",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        int blockId = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            blockId = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return blockId;
    }

    // Returns every (doc_id, disk_block_id) pair currently stored — used to
    // demonstrate disk scheduling over a real batch of pending block reads.
    std::vector<std::pair<int, int>> getAllDocBlocks()
    {
        std::vector<std::pair<int, int>> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT doc_id, disk_block_id FROM documents;", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            out.push_back({sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1)});
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Reads back one document's embedding vector — used to rebuild the
    // in-memory search index (docDB) after a restart.
    std::vector<float> getEmbedding(int docId)
    {
        std::vector<float> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT vector_data FROM embeddings WHERE doc_id = ?;", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const void *blob = sqlite3_column_blob(stmt, 0);
            int bytes = sqlite3_column_bytes(stmt, 0);
            out.resize(bytes / sizeof(float));
            memcpy(out.data(), blob, bytes);
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Everything needed to rebuild one document in memory: id, title, and
    // which disk block its text starts at.
    struct RestoreRow
    {
        int docId;
        std::string title;
        int startBlock;
    };
    std::vector<RestoreRow> getAllDocumentsForRestore()
    {
        std::vector<RestoreRow> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT doc_id, title, disk_block_id FROM documents;", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            out.push_back({sqlite3_column_int(stmt, 0),
                           (const char *)sqlite3_column_text(stmt, 1),
                           sqlite3_column_int(stmt, 2)});
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // The directory a document lives in, joined by name — used so the
    // Documents list can actually show where each file was filed.
    std::string getDirName(int docId)
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "SELECT IFNULL(dir.dir_name, 'root') FROM documents d "
                           "LEFT JOIN directories dir ON d.dir_id = dir.dir_id WHERE d.doc_id = ?;",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        std::string name = "root";
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
            name = (const char *)sqlite3_column_text(stmt, 0);
        sqlite3_finalize(stmt);
        return name;
    }

    void deleteDocument(int docId)
    {
        exec("BEGIN TRANSACTION;");
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "DELETE FROM embeddings WHERE doc_id = ?;", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        sqlite3_prepare_v2(db, "DELETE FROM file_permissions WHERE doc_id = ?;", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        sqlite3_prepare_v2(db, "DELETE FROM documents WHERE doc_id = ?;", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, docId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        exec("COMMIT;");
    }

    // Ensures a default user exists (id=1) so you have something to test with.
    int ensureDefaultUser(const std::string &username = "harsh")
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT user_id FROM users WHERE username = ?;", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        int id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (id != -1)
            return id;

        sqlite3_prepare_v2(db, "INSERT INTO users (username) VALUES (?);", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (int)sqlite3_last_insert_rowid(db);
    }

    // ---- DBMS explorer helpers: expose the real relational data for the UI ----

    struct UserRow
    {
        int id;
        std::string username;
    };
    std::vector<UserRow> listUsers()
    {
        std::vector<UserRow> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT user_id, username FROM users;", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
            out.push_back({sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1)});
        sqlite3_finalize(stmt);
        return out;
    }

    struct DirRow
    {
        int id;
        std::string name;
        int parent;
    };
    std::vector<DirRow> listDirectories()
    {
        std::vector<DirRow> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT dir_id, dir_name, IFNULL(parent_dir_id,-1) FROM directories;", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
            out.push_back({sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1), sqlite3_column_int(stmt, 2)});
        sqlite3_finalize(stmt);
        return out;
    }

    // A join across documents, directories, and users — this is the query
    // a plain vector index can't do, and exactly why a relational layer
    // sits underneath the search: structured lookups across related tables.
    struct DocRow
    {
        int id;
        std::string title;
        std::string owner;
        std::string dir;
        int block;
    };
    std::vector<DocRow> listDocumentsJoined()
    {
        std::vector<DocRow> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "SELECT d.doc_id, d.title, u.username, IFNULL(dir.dir_name,'(root)'), d.disk_block_id "
                           "FROM documents d "
                           "LEFT JOIN users u ON d.owner_id = u.user_id "
                           "LEFT JOIN directories dir ON d.dir_id = dir.dir_id;",
                           -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            out.push_back({sqlite3_column_int(stmt, 0),
                           (const char *)sqlite3_column_text(stmt, 1),
                           sqlite3_column_text(stmt, 2) ? (const char *)sqlite3_column_text(stmt, 2) : "?",
                           (const char *)sqlite3_column_text(stmt, 3),
                           sqlite3_column_int(stmt, 4)});
        }
        sqlite3_finalize(stmt);
        return out;
    }

    struct PermRow
    {
        int docId;
        std::string docTitle;
        std::string username;
        std::string level;
    };
    std::vector<PermRow> listPermissionsJoined()
    {
        std::vector<PermRow> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "SELECT p.doc_id, d.title, u.username, p.access_level "
                           "FROM file_permissions p "
                           "JOIN documents d ON p.doc_id = d.doc_id "
                           "JOIN users u ON p.user_id = u.user_id;",
                           -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            out.push_back({sqlite3_column_int(stmt, 0),
                           (const char *)sqlite3_column_text(stmt, 1),
                           (const char *)sqlite3_column_text(stmt, 2),
                           (const char *)sqlite3_column_text(stmt, 3)});
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Logs one Ask AI exchange: the question, the generated answer, and
    // which documents were actually used as context — a real, queryable
    // record instead of state that only lives in the browser's DOM.
    int insertLog(int userId, const std::string &question, const std::string &answer,
                  const std::vector<int> &contextDocIds)
    {
        exec("BEGIN TRANSACTION;");
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "INSERT INTO user_logs (user_id, question, answer) VALUES (?, ?, ?);",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, userId);
        sqlite3_bind_text(stmt, 2, question.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, answer.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        int logId = (int)sqlite3_last_insert_rowid(db);

        for (int docId : contextDocIds)
        {
            sqlite3_prepare_v2(db, "INSERT INTO log_contexts (log_id, doc_id) VALUES (?, ?);",
                               -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, logId);
            sqlite3_bind_int(stmt, 2, docId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        exec("COMMIT;");
        return logId;
    }

    // Returns a user's past conversation, oldest first, so the frontend can
    // rebuild the chat exactly as it looked before a refresh.
    struct LogRow
    {
        int logId;
        std::string question;
        std::string answer;
        std::string timestamp;
    };
    std::vector<LogRow> listRecentLogs(int userId, int limit = 50)
    {
        std::vector<LogRow> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
                           "SELECT log_id, question, answer, timestamp FROM user_logs "
                           "WHERE user_id = ? ORDER BY log_id DESC LIMIT ?;",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, userId);
        sqlite3_bind_int(stmt, 2, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            out.push_back({sqlite3_column_int(stmt, 0),
                           (const char *)sqlite3_column_text(stmt, 1),
                           (const char *)sqlite3_column_text(stmt, 2),
                           (const char *)sqlite3_column_text(stmt, 3)});
        }
        sqlite3_finalize(stmt);
        std::reverse(out.begin(), out.end()); // oldest first, for natural reading order
        return out;
    }

    // Queries the document_summary VIEW directly, rather than repeating
    // the join as a raw string here — this is the whole point of a view.
    struct SummaryRow
    {
        int docId;
        std::string title;
        std::string owner;
        std::string folder;
        int startBlock;
    };
    std::vector<SummaryRow> queryDocumentSummaryView()
    {
        std::vector<SummaryRow> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT doc_id, title, owner, folder, start_block FROM document_summary;",
                           -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            out.push_back({sqlite3_column_int(stmt, 0),
                           (const char *)sqlite3_column_text(stmt, 1),
                           sqlite3_column_text(stmt, 2) ? (const char *)sqlite3_column_text(stmt, 2) : "?",
                           (const char *)sqlite3_column_text(stmt, 3),
                           sqlite3_column_int(stmt, 4)});
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Reads deletion_audit — every row here was written by the TRIGGER,
    // never by application code, proving the trigger actually fires.
    struct AuditRow
    {
        int auditId;
        int docId;
        std::string title;
        std::string deletedAt;
    };
    std::vector<AuditRow> listDeletionAudit()
    {
        std::vector<AuditRow> out;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT audit_id, doc_id, title, deleted_at FROM deletion_audit ORDER BY audit_id DESC;",
                           -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            out.push_back({sqlite3_column_int(stmt, 0),
                           sqlite3_column_int(stmt, 1),
                           (const char *)sqlite3_column_text(stmt, 2),
                           (const char *)sqlite3_column_text(stmt, 3)});
        }
        sqlite3_finalize(stmt);
        return out;
    }
};