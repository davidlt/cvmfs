#include "catalog_class.h"

#include <assert.h>

extern "C" {
  #include "debug.h"
}

using namespace std;

namespace cvmfs {
  
const uint64_t Catalog::DEFAULT_TTL       = 3600; ///< Default TTL for a catalog is one hour.
const uint64_t Catalog::GROW_EPOCH        = 1199163600;
const int      Catalog::SQLITE_THREAD_MEM = 4; ///< SQLite3 heap limit per thread

Catalog::Catalog(const bool is_root) {
  // initialize mutex
  pthread_mutex_init(&mutex_, NULL);
  
  // initialize catalog hierarchy
  parent_ = NULL;
  
  is_root_ = is_root;
}

bool Catalog::Init(const string &db_file, const uint64_t inode_offset, const bool is_root_catalog) {
  int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READONLY;

  // open database file for reading
  pmesg(D_CATALOG, "opening database file %s", db_file.c_str());
  if (SQLITE_OK != sqlite3_open_v2(db_file.c_str(), &database_, flags, NULL)) {
    pmesg(D_CATALOG, "Cannot open catalog database file %s", db_file.c_str());
    return false;
  }
  sqlite3_extended_result_codes(database_, 1);

  // initialize prepared statements
  listing_statement_ = new ListingLookupSqlStatement(database_);
  path_hash_lookup_statement_ = new PathHashLookupSqlStatement(database_);
  inode_lookup_statement_ = new InodeLookupSqlStatement(database_);
  find_nested_catalog_statement_ = new FindNestedCatalogSqlStatement(database_);
  
  // allocate inode chunk
  inode_offset_ = inode_offset;
  SqlStatement max_row_id_query(database_, "SELECT MAX(rowid) FROM catalog;");
  if (not max_row_id_query.FetchRow()) {
    pmesg(D_CATALOG, "Cannot retrieve maximal row id for database file %s", db_file.c_str());
    return false;
  }
  maximal_row_id_ = max_row_id_query.RetrieveInt64(0);

  // get root prefix
  if (IsRoot()) {
    SqlStatement root_prefix_query(database_, "SELECT value FROM properties WHERE key='root_prefix';");
    if (not root_prefix_query.FetchRow()) {
      pmesg(D_CATALOG, "Cannot retrieve root prefix for root catalog file %s", db_file.c_str());
      return false;
    }
    root_prefix_ = string((char *)root_prefix_query.RetrieveText(0));
    pmesg(D_CATALOG, "found root prefix %s in root catalog file %s", root_prefix_.c_str(), db_file.c_str());
  }
  
  return true;
}

Catalog::~Catalog() {
  delete listing_statement_;
  delete path_hash_lookup_statement_;
  delete inode_lookup_statement_;
  delete find_nested_catalog_statement_;
  
  sqlite3_close(database_);
  
  pthread_mutex_destroy(&mutex_);
}

bool Catalog::Lookup(const uint64_t inode, DirectoryEntry *entry) const {
  bool found = false;
  
  Lock();
  inode_lookup_statement_->BindInode(inode);
  if (inode_lookup_statement_->FetchRow()) {
    *entry = inode_lookup_statement_->GetDirectoryEntry();
    found = true;
  }
  inode_lookup_statement_->Reset();
  Unlock();
  
  return found;
}

bool Catalog::Lookup(const hash::t_md5 &path_hash, DirectoryEntry *entry) const {
  bool found = false;
  
  Lock();
  path_hash_lookup_statement_->BindPathHash(path_hash);
	if (path_hash_lookup_statement_->FetchRow()) {
    *entry = path_hash_lookup_statement_->GetDirectoryEntry();
    found = EnsureConsistencyOfDirectoryEntry(path_hash, entry);
	}
  path_hash_lookup_statement_->Reset();
  Unlock();
  
  return found;
}

bool Catalog::Listing(const inode_t inode, DirectoryEntryList *listing) const {
  assert (false); // currently not implemented
  return false;
}

bool Catalog::Listing(const hash::t_md5 &path_hash, DirectoryEntryList *listing) const {
  Lock();
  listing_statement_->BindPathHash(path_hash);
  while (listing_statement_->FetchRow()) {
    DirectoryEntry entry = listing_statement_->GetDirectoryEntry();
    EnsureConsistencyOfDirectoryEntry(path_hash, &entry);
    listing->push_back(entry);
  }
  listing_statement_->Reset();
  Unlock();
  
  return true;
}

bool Catalog::EnsureConsistencyOfDirectoryEntry(const hash::t_md5 &path_hash, DirectoryEntry *entry) const {
  // if we encounter the root entry of a nested catalog the inode has to be
  // changed to the mount point of this nested catalog. This entry must be
  // listed in the parent catalog under the same path hash.
	if (entry->IsNestedCatalogRoot() && not this->IsRoot()) {
    DirectoryEntry nestedRootMountpoint;
    bool foundMountpoint = parent_->Lookup(path_hash, &nestedRootMountpoint);
    
    if (not foundMountpoint) {
      pmesg(D_CATALOG, "FATAL: mount point of nested catalog root could not be found in parent catalog");
      return false;
    } else {  
      entry->set_inode(nestedRootMountpoint.inode());
    }
	}
	
  return true;
}

} // namespace cvmfs
