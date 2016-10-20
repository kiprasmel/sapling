/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "TreeInode.h"

#include "EdenMount.h"
#include "Overlay.h"
#include "TreeEntryFileInode.h"
#include "TreeInodeDirHandle.h"
#include "eden/fs/inodes/EdenMount.h"
#include "eden/fs/inodes/FileData.h"
#include "eden/fs/journal/JournalDelta.h"
#include "eden/fs/model/Tree.h"
#include "eden/fs/model/TreeEntry.h"
#include "eden/fs/store/ObjectStore.h"
#include "eden/fuse/MountPoint.h"
#include "eden/fuse/RequestData.h"
#include "eden/utils/PathFuncs.h"

namespace facebook {
namespace eden {

TreeInode::TreeInode(
    EdenMount* mount,
    std::unique_ptr<Tree>&& tree,
    TreeInode::Entry* entry,
    fuse_ino_t parent,
    fuse_ino_t ino)
    : DirInode(ino),
      mount_(mount),
      contents_(buildDirFromTree(tree.get())),
      entry_(entry),
      parent_(parent) {
  DCHECK(ino == FUSE_ROOT_ID || entry_ != nullptr)
      << "only the root dir can have a null entry_";
}

TreeInode::TreeInode(
    EdenMount* mount,
    TreeInode::Dir&& dir,
    TreeInode::Entry* entry,
    fuse_ino_t parent,
    fuse_ino_t ino)
    : DirInode(ino),
      mount_(mount),
      contents_(std::move(dir)),
      entry_(entry),
      parent_(parent) {
  DCHECK(ino == FUSE_ROOT_ID || entry_ != nullptr)
      << "only the root dir can have a null entry_";
}

TreeInode::~TreeInode() {}

folly::Future<fusell::Dispatcher::Attr> TreeInode::getattr() {
  return getAttrLocked(&*contents_.rlock());
}

fusell::Dispatcher::Attr TreeInode::getAttrLocked(const Dir* contents) {
  fusell::Dispatcher::Attr attr(getMount()->getMountPoint());

  attr.st.st_mode = S_IFDIR | 0755;
  attr.st.st_ino = getNodeId();
  // TODO: set atime, mtime, and ctime

  // For directories, nlink is the number of entries including the
  // "." and ".." links.
  attr.st.st_nlink = contents->entries.size() + 2;
  return attr;
}

std::shared_ptr<fusell::InodeBase> TreeInode::getChildByNameLocked(
    const Dir* contents,
    PathComponentPiece name) {
  auto iter = contents->entries.find(name);
  if (iter == contents->entries.end()) {
    folly::throwSystemErrorExplicit(ENOENT);
  }

  // Only allocate an inode number once we know that the entry exists!
  auto node = getNameMgr()->getNodeByName(getNodeId(), name);
  auto entry = iter->second.get();

  if (S_ISDIR(entry->mode)) {
    if (!entry->materialized && entry->hash) {
      auto tree = getStore()->getTree(entry->hash.value());
      return std::make_shared<TreeInode>(
          mount_, std::move(tree), entry, getNodeId(), node->getNodeId());
    }

    // No corresponding TreeEntry, this exists only in the overlay.
    auto targetName = getNameMgr()->resolvePathToNode(node->getNodeId());
    auto overlayDir = getOverlay()->loadOverlayDir(targetName);
    DCHECK(overlayDir) << "missing overlay for " << targetName;
    return std::make_shared<TreeInode>(
        mount_,
        std::move(overlayDir.value()),
        entry,
        getNodeId(),
        node->getNodeId());
  }

  return std::make_shared<TreeEntryFileInode>(
      node->getNodeId(),
      std::static_pointer_cast<TreeInode>(shared_from_this()),
      entry);
}

folly::Future<std::shared_ptr<fusell::InodeBase>> TreeInode::getChildByName(
    PathComponentPiece namepiece) {
  auto contents = contents_.rlock();
  return getChildByNameLocked(&*contents, namepiece);
}

fuse_ino_t TreeInode::getParent() const {
  return parent_;
}

fuse_ino_t TreeInode::getInode() const {
  return getNodeId();
}

folly::Future<std::shared_ptr<fusell::DirHandle>> TreeInode::opendir(
    const struct fuse_file_info&) {
  return std::make_shared<TreeInodeDirHandle>(
      std::static_pointer_cast<TreeInode>(shared_from_this()));
}

/* If we don't yet have an overlay entry for this portion of the tree,
 * populate it from the Tree.  In order to materialize a dir we have
 * to also materialize its parents. */
void TreeInode::materializeDirAndParents() {
  if (contents_.rlock()->materialized) {
    // Already materialized, all done!
    return;
  }

  // Ensure that our parent(s) are materialized.  We can't go higher
  // than the root inode though.
  if (getNodeId() != FUSE_ROOT_ID) {
    auto parentInode = std::dynamic_pointer_cast<TreeInode>(
        getMount()->getMountPoint()->getDispatcher()->getDirInode(parent_));
    DCHECK(parentInode) << "must always have a TreeInode parent";
    // and get it to materialize
    parentInode->materializeDirAndParents();
  }

  // Atomically, wrt. to concurrent callers, cause the materialized flag
  // to be set to true both for this directory and for our entry in the
  // parent directory in the in-memory state.
  bool updateParent = contents_.withWLockPtr([&](auto wlock) {
    if (wlock->materialized) {
      // Someone else materialized it in the meantime
      return false;
    }

    auto myname = this->getNameMgr()->resolvePathToNode(this->getNodeId());

    auto overlay = this->getOverlay();
    auto dirPath = overlay->getContentDir() + myname;
    if (::mkdir(dirPath.c_str(), 0755) != 0 && errno != EEXIST) {
      folly::throwSystemError("while materializing, mkdir: ", dirPath);
    }
    wlock->materialized = true;
    overlay->saveOverlayDir(myname, &*wlock);

    if (entry_ && !entry_->materialized) {
      entry_->materialized = true;
      return true;
    }

    return false;
  });

  // If we just set materialized on the entry, we need to arrange for that
  // state to be saved to disk.  This is not atomic wrt. to the property
  // change, but definitely does not have a lock-order-acquisition deadlock.
  // This means that there is a small window of time where our in-memory and
  // on-disk state for the overlay are not in sync.
  if (updateParent) {
    auto parentInode = std::dynamic_pointer_cast<TreeInode>(
        getMount()->getMountPoint()->getDispatcher()->getDirInode(parent_));
    auto parentName = getNameMgr()->resolvePathToNode(parentInode->getNodeId());
    getOverlay()->saveOverlayDir(parentName, &*parentInode->contents_.wlock());
  }
}

TreeInode::Dir TreeInode::buildDirFromTree(const Tree* tree) {
  // Now build out the Dir based on what we know.
  Dir dir;
  if (!tree) {
    // There's no associated Tree, so we have to persist this to the
    // overlay storage area
    dir.materialized = true;
    return dir;
  }

  auto myname = getNameMgr()->resolvePathToNode(getNodeId());
  dir.treeHash = tree->getHash();
  for (const auto& treeEntry : tree->getTreeEntries()) {
    Entry entry;

    entry.hash = treeEntry.getHash();
    entry.mode = treeEntry.getMode();

    dir.entries.emplace(
        treeEntry.getName(), std::make_unique<Entry>(std::move(entry)));
  }
  return dir;
}

folly::Future<fusell::DirInode::CreateResult>
TreeInode::create(PathComponentPiece name, mode_t mode, int flags) {
  // Figure out the relative path to this inode.
  auto myname = getNameMgr()->resolvePathToNode(getNodeId());

  // Compute the effective name of the node they want to create.
  auto targetName = myname + name;
  std::shared_ptr<fusell::FileHandle> handle;
  std::shared_ptr<TreeEntryFileInode> inode;
  std::shared_ptr<fusell::InodeNameManager::Node> node;

  materializeDirAndParents();

  auto filePath = getOverlay()->getContentDir() + targetName;

  // We need to scope the write lock as the getattr call below implicitly
  // wants to acquire a read lock.
  contents_.withWLock([&](auto& contents) {
    // Since we will move this file into the underlying file data, we
    // take special care to ensure that it is opened read-write
    folly::File file(
        filePath.c_str(),
        O_RDWR | O_CREAT | (flags & ~(O_RDONLY | O_WRONLY)),
        0600);

    // Record the new entry
    auto& entry = contents.entries[name];
    entry = std::make_unique<Entry>();
    entry->materialized = true;

    struct stat st;
    folly::checkUnixError(::fstat(file.fd(), &st));
    entry->mode = st.st_mode;

    // Generate an inode number for this new entry.
    node = this->getNameMgr()->getNodeByName(this->getNodeId(), name);

    // build a corresponding TreeEntryFileInode
    inode = std::make_shared<TreeEntryFileInode>(
        node->getNodeId(),
        std::static_pointer_cast<TreeInode>(this->shared_from_this()),
        entry.get(),
        std::move(file));

    // The kernel wants an open operation to return the inode,
    // the file handle and some attribute information.
    // Let's open a file handle now.
    handle = inode->finishCreate();

    this->getOverlay()->saveOverlayDir(myname, &contents);
  });

  getMount()->getJournal().wlock()->addDelta(
      std::make_unique<JournalDelta>(JournalDelta{targetName}));

  // Now that we have the file handle, let's look up the attributes.
  auto getattrResult = handle->getattr();
  return getattrResult.then(
      [ =, handle = std::move(handle) ](fusell::Dispatcher::Attr attr) mutable {
        fusell::DirInode::CreateResult result(getMount()->getMountPoint());

        // Return all of the results back to the kernel.
        result.inode = inode;
        result.file = std::move(handle);
        result.attr = attr;
        result.node = node;

        return result;
      });
}

bool TreeInode::canForget() {
  // We can't let this inode be forgotten while it is materialized,
  // as we hold the source of truth about this entry.
  return !contents_.rlock()->materialized;
}

folly::Future<fuse_entry_param> TreeInode::mkdir(
    PathComponentPiece name,
    mode_t mode) {
  // Figure out the relative path to this inode.
  auto myname = getNameMgr()->resolvePathToNode(getNodeId());
  auto targetName = myname + name;
  // Compute the effective name of the node they want to create.
  materializeDirAndParents();

  contents_.withWLock([&](auto& contents) {
    auto entIter = contents.entries.find(name);
    if (entIter != contents.entries.end()) {
      folly::throwSystemErrorExplicit(
          EEXIST, "mkdir: ", targetName, " already exists in the overlay");
    }
    auto overlay = this->getOverlay();

    auto dirPath = overlay->getContentDir() + targetName;

    folly::checkUnixError(
        ::mkdir(dirPath.c_str(), mode), "mkdir: ", dirPath, " mode=", mode);

    // We succeeded, let's update our state
    struct stat st;
    folly::checkUnixError(::lstat(dirPath.c_str(), &st));

    auto entry = std::make_unique<Entry>();
    entry->mode = st.st_mode;
    entry->materialized = true;

    contents.entries.emplace(name, std::move(entry));
    overlay->saveOverlayDir(myname, &contents);

    // Create the overlay entry for this dir before the lookup call
    // below tries to load it (and fails)
    Dir emptyDir;
    emptyDir.materialized = true;
    overlay->saveOverlayDir(targetName, &emptyDir);
  });

  getMount()->getJournal().wlock()->addDelta(
      std::make_unique<JournalDelta>(JournalDelta{targetName}));

  // Look up the inode for this new dir and return its entry info.
  return getMount()->getMountPoint()->getDispatcher()->lookup(
      getNodeId(), name);
}

folly::Future<folly::Unit> TreeInode::unlink(PathComponentPiece name) {
  // Compute the full name of the node they want to remove.
  auto myname = getNameMgr()->resolvePathToNode(getNodeId());
  auto targetName = myname + name;

  // Check pre-conditions with a read lock before we materialize anything
  // in case we're processing spurious unlink for a non-existent entry;
  // we don't want to materialize part of a tree if we're not actually
  // going to do any work in it.
  contents_.withRLock([&](const auto& contents) {
    auto entIter = contents.entries.find(name);
    if (entIter == contents.entries.end()) {
      folly::throwSystemErrorExplicit(
          ENOENT, "unlink: ", targetName, " does not exist");
    }
    if (S_ISDIR(entIter->second->mode)) {
      folly::throwSystemErrorExplicit(
          EISDIR, "unlink: ", targetName, " is a directory");
    }
  });

  materializeDirAndParents();

  contents_.withWLock([&](auto& contents) {
    // Re-check the pre-conditions in case we raced
    auto entIter = contents.entries.find(name);
    if (entIter == contents.entries.end()) {
      folly::throwSystemErrorExplicit(
          ENOENT, "unlink: ", targetName, " does not exist");
    }

    auto& ent = entIter->second;
    if (S_ISDIR(ent->mode)) {
      folly::throwSystemErrorExplicit(
          EISDIR, "unlink: ", targetName, " is a directory");
    }

    auto overlay = this->getOverlay();

    if (ent->materialized) {
      auto filePath = overlay->getContentDir() + targetName;
      folly::checkUnixError(::unlink(filePath.c_str()), "unlink: ", filePath);
    }

    // And actually remove it
    contents.entries.erase(entIter);
    overlay->saveOverlayDir(myname, &contents);
  });

  getMount()->getJournal().wlock()->addDelta(
      std::make_unique<JournalDelta>(JournalDelta{targetName}));

  return folly::Unit{};
}

std::shared_ptr<fusell::InodeBase> TreeInode::lookupChildByNameLocked(
    const Dir* contents,
    PathComponentPiece name) {
  auto mountPoint = getMount()->getMountPoint();
  auto dispatcher = mountPoint->getDispatcher();
  auto mgr = mountPoint->getNameMgr();

  auto node = mgr->getNodeByName(getNodeId(), name, false);

  if (node) {
    return dispatcher->getInode(node->getNodeId(), true);
  }

  auto child = getChildByNameLocked(contents, name);

  node = mgr->getNodeById(child->getNodeId());
  dispatcher->recordInode(child);

  return child;
}

folly::Future<folly::Unit> TreeInode::rmdir(PathComponentPiece name) {
  // Compute the full name of the node they want to remove.
  auto myname = getNameMgr()->resolvePathToNode(getNodeId());
  auto targetName = myname + name;

  // Check pre-conditions with a read lock before we materialize anything
  // in case we're processing spurious unlink for a non-existent entry;
  // we don't want to materialize part of a tree if we're not actually
  // going to do any work in it.
  contents_.withRLock([&](const auto& contents) {
    auto entIter = contents.entries.find(name);
    if (entIter == contents.entries.end()) {
      folly::throwSystemErrorExplicit(
          ENOENT, "rmdir: ", targetName, " does not exist");
    }
    if (!S_ISDIR(entIter->second->mode)) {
      folly::throwSystemErrorExplicit(
          EISDIR, "rmdir: ", targetName, " is not a directory");
    }
    auto targetInode = this->lookupChildByNameLocked(&contents, name);
    if (!targetInode) {
      folly::throwSystemErrorExplicit(
          EIO,
          "rmdir: ",
          targetName,
          " is supposed to exist but didn't resolve to an inode object");
    }
    auto targetDir = std::dynamic_pointer_cast<TreeInode>(targetInode);
    if (!targetDir) {
      folly::throwSystemErrorExplicit(
          EIO,
          "rmdir: ",
          targetName,
          " is supposed to be a dir but didn't resolve to a TreeInode object");
    }

    targetDir->contents_.withRLock([&](const auto& targetContents) {
      if (!targetContents.entries.empty()) {
        folly::throwSystemErrorExplicit(
            ENOTEMPTY, "rmdir: ", targetName, " is not empty");
      }
    });
  });

  materializeDirAndParents();

  contents_.withWLock([&](auto& contents) {
    // Re-check the pre-conditions in case we raced
    auto entIter = contents.entries.find(name);
    if (entIter == contents.entries.end()) {
      folly::throwSystemErrorExplicit(
          ENOENT, "rmdir: ", targetName, " does not exist");
    }

    auto& ent = entIter->second;
    if (!S_ISDIR(ent->mode)) {
      folly::throwSystemErrorExplicit(
          EISDIR, "rmdir: ", targetName, " is not a directory");
    }
    auto targetInode = this->lookupChildByNameLocked(&contents, name);
    if (!targetInode) {
      folly::throwSystemErrorExplicit(
          EIO,
          "rmdir: ",
          targetName,
          " is supposed to exist but didn't resolve to an inode object");
    }
    auto targetDir = std::dynamic_pointer_cast<TreeInode>(targetInode);
    if (!targetDir) {
      folly::throwSystemErrorExplicit(
          EIO,
          "rmdir: ",
          targetName,
          " is supposed to be a dir but didn't resolve to a TreeInode object");
    }
    if (!targetDir->contents_.rlock()->entries.empty()) {
      folly::throwSystemErrorExplicit(
          ENOTEMPTY, "rmdir: ", targetName, " is not empty");
    }

    auto overlay = this->getOverlay();
    if (ent->materialized) {
      auto dirPath = overlay->getContentDir() + targetName;
      folly::checkUnixError(::rmdir(dirPath.c_str()), "rmdir: ", dirPath);
    }

    // And actually remove it
    contents.entries.erase(entIter);
    overlay->saveOverlayDir(myname, &contents);
    overlay->removeOverlayDir(targetName);
  });

  getMount()->getJournal().wlock()->addDelta(
      std::make_unique<JournalDelta>(JournalDelta{targetName}));

  return folly::Unit{};
}

void TreeInode::renameHelper(
    Dir* sourceContents,
    RelativePathPiece sourceName,
    Dir* destContents,
    RelativePathPiece destName) {
  auto sourceEntIter = sourceContents->entries.find(sourceName.basename());
  if (sourceEntIter == sourceContents->entries.end()) {
    folly::throwSystemErrorExplicit(
        ENOENT, "rename: source file ", sourceName, " does not exist");
  }

  auto destEntIter = destContents->entries.find(destName.basename());

  if (mode_to_dtype(sourceEntIter->second->mode) == dtype_t::Dir &&
      destEntIter != destContents->entries.end()) {
    // When renaming a directory, the destination must either not exist or
    // it must be an empty directory
    if (mode_to_dtype(destEntIter->second->mode) != dtype_t::Dir) {
      folly::throwSystemErrorExplicit(
          ENOTDIR,
          "attempted to rename dir ",
          sourceName,
          " to existing name ",
          destName,
          " but the latter is not a directory");
    }

    auto targetInode =
        lookupChildByNameLocked(destContents, destName.basename());
    auto destDir = std::dynamic_pointer_cast<TreeInode>(targetInode);
    if (!destDir) {
      folly::throwSystemErrorExplicit(
          EIO, "inconsistency between contents and inodes objects");
    }

    if (!destDir->contents_.rlock()->entries.empty()) {
      folly::throwSystemErrorExplicit(
          ENOTEMPTY,
          "attempted to rename dir ",
          sourceName,
          " to dir ",
          destName,
          " but the latter is not a empty directory");
    }
  }

  auto contentDir = getOverlay()->getContentDir();
  auto absoluteSourcePath = contentDir + sourceName;
  auto absoluteDestPath = contentDir + destName;

  // If we haven't actually materialized it yet, the rename() call will
  // fail.  So don't try that.
  if (sourceEntIter->second->materialized) {
    folly::checkUnixError(
        ::rename(absoluteSourcePath.c_str(), absoluteDestPath.c_str()),
        "rename ",
        absoluteSourcePath,
        " to ",
        absoluteDestPath,
        " failed");
  }

  // Success.
  // Update the destination with the source data (this copies in the hash if
  // it happens to be set).
  auto& destEnt = destContents->entries[destName.basename()];
  // Note: sourceEntIter may have been invalidated by the line above in the
  // case that the source and destination dirs are the same.  We need to
  // recompute that iterator now to be safe.
  sourceEntIter = sourceContents->entries.find(sourceName.basename());

  // We want to move in the data from the source.
  destEnt = std::move(sourceEntIter->second);

  // Now remove the source information
  sourceContents->entries.erase(sourceEntIter);

  getOverlay()->saveOverlayDir(sourceName.dirname(), sourceContents);
  if (sourceContents != destContents) {
    // Don't saved the same thing twice if the rename is within the
    // same directory.
    getOverlay()->saveOverlayDir(destName.dirname(), destContents);
  }
}

folly::Future<folly::Unit> TreeInode::rename(
    PathComponentPiece name,
    std::shared_ptr<DirInode> newParent,
    PathComponentPiece newName) {
  auto targetDir = std::dynamic_pointer_cast<TreeInode>(newParent);
  if (!targetDir) {
    // This probably can't happen, but it is better to be safe than sorry.
    folly::throwSystemErrorExplicit(EXDEV, "target dir is not a TreeInode");
  }

  auto nameMgr = getNameMgr();
  auto sourceName = nameMgr->resolvePathToNode(getNodeId()) + name;
  auto targetName =
      nameMgr->resolvePathToNode(targetDir->getNodeId()) + newName;

  // Check pre-conditions with a read lock before we materialize anything
  // in case we're processing spurious rename for a non-existent entry;
  // we don't want to materialize part of a tree if we're not actually
  // going to do any work in it.
  // There are some more complex pre-conditions that we'd like to check
  // before materializing, but we cannot do so in a race free manner
  // without locking each of the associated objects.  The existence
  // check is sufficient to avoid the majority of the potentially
  // wasted effort.
  contents_.withRLock([&](const auto& contents) {
    auto entIter = contents.entries.find(name);
    if (entIter == contents.entries.end()) {
      folly::throwSystemErrorExplicit(
          ENOENT, "rename: source file ", sourceName, " does not exist");
    }
  });

  materializeDirAndParents();

  // Can't use SYNCHRONIZED_DUAL for both cases, as we'd self-deadlock by trying
  // to wlock the same thing twice
  if (targetDir.get() == this) {
    contents_.withWLock([&](auto& contents) {
      this->renameHelper(&contents, sourceName, &contents, targetName);
    });
  } else {
    targetDir->materializeDirAndParents();

    SYNCHRONIZED_DUAL(
        sourceContents, contents_, destContents, targetDir->contents_) {
      renameHelper(&sourceContents, sourceName, &destContents, targetName);
    }
  }

  getMount()->getJournal().wlock()->addDelta(
      std::make_unique<JournalDelta>(JournalDelta{sourceName, targetName}));
  return folly::Unit{};
}

EdenMount* TreeInode::getMount() const {
  return mount_;
}

fusell::InodeNameManager* TreeInode::getNameMgr() const {
  return mount_->getMountPoint()->getNameMgr();
}

ObjectStore* TreeInode::getStore() const {
  return mount_->getObjectStore();
}

const std::shared_ptr<Overlay>& TreeInode::getOverlay() const {
  return mount_->getOverlay();
}

void TreeInode::performCheckout(const Hash& hash) {
  throw std::runtime_error("not yet implemented");
}
}
}
