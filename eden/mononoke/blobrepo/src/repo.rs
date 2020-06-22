/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

use super::utils::DangerousOverride;
use crate::errors::*;
use anyhow::{format_err, Error};
use blobstore::Blobstore;
use bonsai_git_mapping::BonsaiGitMapping;
use bonsai_globalrev_mapping::{BonsaiGlobalrevMapping, BonsaisOrGlobalrevs};
use bonsai_hg_mapping::BonsaiHgMapping;
use bookmarks::{
    self, Bookmark, BookmarkName, BookmarkPrefix, BookmarkUpdateLogEntry, BookmarkUpdateReason,
    Bookmarks, Freshness,
};
use cacheblob::LeaseOps;
use changeset_fetcher::{ChangesetFetcher, SimpleChangesetFetcher};
use changesets::{ChangesetInsert, Changesets};
use cloned::cloned;
use context::CoreContext;
use filenodes::Filenodes;
use filestore::FilestoreConfig;
use futures::{compat::Future01CompatExt, future::FutureExt as NewFutureExt};
use futures_ext::{BoxFuture, FutureExt};
use futures_old::future::{loop_fn, ok, Future, Loop};
use futures_old::stream::{self, FuturesUnordered, Stream};
use mercurial_types::Globalrev;
use metaconfig_types::DerivedDataConfig;
use mononoke_types::{
    BlobstoreValue, BonsaiChangeset, ChangesetId, Generation, MononokeId, RepositoryId, Timestamp,
};
use phases::{HeadsFetcher, Phases, SqlPhasesFactory};
use repo_blobstore::{RepoBlobstore, RepoBlobstoreArgs};
use stats::prelude::*;
use std::{
    collections::{HashMap, HashSet},
    sync::Arc,
};
use topo_sort::sort_topological;
use type_map::TypeMap;

define_stats! {
    prefix = "mononoke.blobrepo";
    get_bonsai_heads_maybe_stale: timeseries(Rate, Sum),
    get_bonsai_publishing_bookmarks_maybe_stale: timeseries(Rate, Sum),
    get_raw_hg_content: timeseries(Rate, Sum),
    get_changesets: timeseries(Rate, Sum),
    get_heads_maybe_stale: timeseries(Rate, Sum),
    changeset_exists: timeseries(Rate, Sum),
    changeset_exists_by_bonsai: timeseries(Rate, Sum),
    many_changesets_exists: timeseries(Rate, Sum),
    get_changeset_parents: timeseries(Rate, Sum),
    get_changeset_parents_by_bonsai: timeseries(Rate, Sum),
    get_hg_file_copy_from_blobstore: timeseries(Rate, Sum),
    get_hg_from_bonsai_changeset: timeseries(Rate, Sum),
    generate_hg_from_bonsai_changeset: timeseries(Rate, Sum),
    generate_hg_from_bonsai_total_latency_ms: histogram(100, 0, 10_000, Average; P 50; P 75; P 90; P 95; P 99),
    generate_hg_from_bonsai_single_latency_ms: histogram(100, 0, 10_000, Average; P 50; P 75; P 90; P 95; P 99),
    generate_hg_from_bonsai_generated_commit_num: histogram(1, 0, 20, Average; P 50; P 75; P 90; P 95; P 99),
    get_bookmark: timeseries(Rate, Sum),
    get_bookmarks_by_prefix_maybe_stale: timeseries(Rate, Sum),
    get_publishing_bookmarks_maybe_stale: timeseries(Rate, Sum),
    get_pull_default_bookmarks_maybe_stale: timeseries(Rate, Sum),
    get_bonsai_from_hg: timeseries(Rate, Sum),
    get_hg_bonsai_mapping: timeseries(Rate, Sum),
    update_bookmark_transaction: timeseries(Rate, Sum),
    get_linknode: timeseries(Rate, Sum),
    get_linknode_opt: timeseries(Rate, Sum),
    get_all_filenodes: timeseries(Rate, Sum),
    get_generation_number: timeseries(Rate, Sum),
    create_changeset: timeseries(Rate, Sum),
    create_changeset_compute_cf: timeseries("create_changeset.compute_changed_files"; Rate, Sum),
    create_changeset_expected_cf: timeseries("create_changeset.expected_changed_files"; Rate, Sum),
    create_changeset_cf_count: timeseries("create_changeset.changed_files_count"; Average, Sum),
}

pub struct BlobRepo {
    blobstore: RepoBlobstore,
    bookmarks: Arc<dyn Bookmarks>,
    changesets: Arc<dyn Changesets>,
    bonsai_git_mapping: Arc<dyn BonsaiGitMapping>,
    bonsai_globalrev_mapping: Arc<dyn BonsaiGlobalrevMapping>,
    repoid: RepositoryId,
    // Returns new ChangesetFetcher that can be used by operation that work with commit graph
    // (for example, revsets).
    changeset_fetcher_factory:
        Arc<dyn Fn() -> Arc<dyn ChangesetFetcher + Send + Sync> + Send + Sync>,
    derived_data_lease: Arc<dyn LeaseOps>,
    filestore_config: FilestoreConfig,
    phases_factory: SqlPhasesFactory,
    derived_data_config: DerivedDataConfig,
    reponame: String,
    attributes: Arc<TypeMap>,
}

impl BlobRepo {
    /// Create new `BlobRepo` object.
    ///
    /// Avoid using this constructor directly as it requires properly initialized `attributes`
    /// argument. Instead use `blobrepo_factory::*` functions.
    pub fn new_dangerous(
        bookmarks: Arc<dyn Bookmarks>,
        blobstore_args: RepoBlobstoreArgs,
        changesets: Arc<dyn Changesets>,
        bonsai_git_mapping: Arc<dyn BonsaiGitMapping>,
        bonsai_globalrev_mapping: Arc<dyn BonsaiGlobalrevMapping>,
        derived_data_lease: Arc<dyn LeaseOps>,
        filestore_config: FilestoreConfig,
        phases_factory: SqlPhasesFactory,
        derived_data_config: DerivedDataConfig,
        reponame: String,
        attributes: Arc<TypeMap>,
    ) -> Self {
        let (blobstore, repoid) = blobstore_args.into_blobrepo_parts();

        let changeset_fetcher_factory = {
            cloned!(changesets, repoid);
            move || {
                let res: Arc<dyn ChangesetFetcher + Send + Sync> = Arc::new(
                    SimpleChangesetFetcher::new(changesets.clone(), repoid.clone()),
                );
                res
            }
        };

        BlobRepo {
            bookmarks,
            blobstore,
            changesets,
            bonsai_git_mapping,
            bonsai_globalrev_mapping,
            repoid,
            changeset_fetcher_factory: Arc::new(changeset_fetcher_factory),
            derived_data_lease,
            filestore_config,
            phases_factory,
            derived_data_config,
            reponame,
            attributes,
        }
    }

    pub fn get_attribute<T: ?Sized + Send + Sync + 'static>(&self) -> Option<&Arc<T>> {
        self.attributes.get::<T>()
    }

    /// Get Bonsai changesets for Mercurial heads, which we approximate as Publishing Bonsai
    /// Bookmarks. Those will be served from cache, so they might be stale.
    pub fn get_bonsai_heads_maybe_stale(
        &self,
        ctx: CoreContext,
    ) -> impl Stream<Item = ChangesetId, Error = Error> {
        STATS::get_bonsai_heads_maybe_stale.add_value(1);
        self.bookmarks
            .list_publishing_by_prefix(
                ctx,
                &BookmarkPrefix::empty(),
                self.get_repoid(),
                Freshness::MaybeStale,
            )
            .map(|(_, cs_id)| cs_id)
    }

    /// List all publishing Bonsai bookmarks.
    pub fn get_bonsai_publishing_bookmarks_maybe_stale(
        &self,
        ctx: CoreContext,
    ) -> impl Stream<Item = (Bookmark, ChangesetId), Error = Error> {
        STATS::get_bonsai_publishing_bookmarks_maybe_stale.add_value(1);
        self.bookmarks.list_publishing_by_prefix(
            ctx,
            &BookmarkPrefix::empty(),
            self.repoid,
            Freshness::MaybeStale,
        )
    }

    /// Get bookmarks by prefix, they will be read from replica, so they might be stale.
    pub fn get_bonsai_bookmarks_by_prefix_maybe_stale(
        &self,
        ctx: CoreContext,
        prefix: &BookmarkPrefix,
        max: u64,
    ) -> impl Stream<Item = (Bookmark, ChangesetId), Error = Error> {
        STATS::get_bookmarks_by_prefix_maybe_stale.add_value(1);
        self.bookmarks.list_all_by_prefix(
            ctx.clone(),
            prefix,
            self.repoid,
            Freshness::MaybeStale,
            max,
        )
    }

    pub fn changeset_exists_by_bonsai(
        &self,
        ctx: CoreContext,
        changesetid: ChangesetId,
    ) -> BoxFuture<bool, Error> {
        STATS::changeset_exists_by_bonsai.add_value(1);
        let changesetid = changesetid.clone();
        let repo = self.clone();
        let repoid = self.repoid.clone();

        repo.changesets
            .get(ctx, repoid, changesetid)
            .map(|res| res.is_some())
            .boxify()
    }

    pub fn get_changeset_parents_by_bonsai(
        &self,
        ctx: CoreContext,
        changesetid: ChangesetId,
    ) -> impl Future<Item = Vec<ChangesetId>, Error = Error> {
        STATS::get_changeset_parents_by_bonsai.add_value(1);
        let repo = self.clone();
        let repoid = self.repoid.clone();

        repo.changesets
            .get(ctx, repoid, changesetid)
            .and_then(move |maybe_bonsai| {
                maybe_bonsai.ok_or(ErrorKind::BonsaiNotFound(changesetid).into())
            })
            .map(|bonsai| bonsai.parents)
    }

    pub fn get_bonsai_bookmark(
        &self,
        ctx: CoreContext,
        name: &BookmarkName,
    ) -> BoxFuture<Option<ChangesetId>, Error> {
        STATS::get_bookmark.add_value(1);
        self.bookmarks.get(ctx, name, self.repoid)
    }

    pub fn bonsai_git_mapping(&self) -> &Arc<dyn BonsaiGitMapping> {
        &self.bonsai_git_mapping
    }

    pub fn bonsai_globalrev_mapping(&self) -> &Arc<dyn BonsaiGlobalrevMapping> {
        &self.bonsai_globalrev_mapping
    }

    pub fn get_bonsai_from_globalrev(
        &self,
        globalrev: Globalrev,
    ) -> BoxFuture<Option<ChangesetId>, Error> {
        self.bonsai_globalrev_mapping
            .get_bonsai_from_globalrev(self.repoid, globalrev)
    }

    pub fn get_globalrev_from_bonsai(
        &self,
        bcs: ChangesetId,
    ) -> BoxFuture<Option<Globalrev>, Error> {
        self.bonsai_globalrev_mapping
            .get_globalrev_from_bonsai(self.repoid, bcs)
    }

    pub fn get_bonsai_globalrev_mapping(
        &self,
        bonsai_or_globalrev_ids: impl Into<BonsaisOrGlobalrevs>,
    ) -> BoxFuture<Vec<(ChangesetId, Globalrev)>, Error> {
        self.bonsai_globalrev_mapping
            .get(self.repoid, bonsai_or_globalrev_ids.into())
            .map(|result| {
                result
                    .into_iter()
                    .map(|entry| (entry.bcs_id, entry.globalrev))
                    .collect()
            })
            .boxify()
    }

    pub fn list_bookmark_log_entries(
        &self,
        ctx: CoreContext,
        name: BookmarkName,
        max_rec: u32,
        offset: Option<u32>,
        freshness: Freshness,
    ) -> impl Stream<Item = (Option<ChangesetId>, BookmarkUpdateReason, Timestamp), Error = Error>
    {
        self.bookmarks.list_bookmark_log_entries(
            ctx.clone(),
            name,
            self.repoid,
            max_rec,
            offset,
            freshness,
        )
    }

    pub fn read_next_bookmark_log_entries(
        &self,
        ctx: CoreContext,
        id: u64,
        limit: u64,
        freshness: Freshness,
    ) -> impl Stream<Item = BookmarkUpdateLogEntry, Error = Error> {
        self.bookmarks
            .read_next_bookmark_log_entries(ctx, id, self.get_repoid(), limit, freshness)
    }

    pub fn count_further_bookmark_log_entries(
        &self,
        ctx: CoreContext,
        id: u64,
        exclude_reason: Option<BookmarkUpdateReason>,
    ) -> impl Future<Item = u64, Error = Error> {
        self.bookmarks.count_further_bookmark_log_entries(
            ctx,
            id,
            self.get_repoid(),
            exclude_reason,
        )
    }

    pub fn update_bookmark_transaction(&self, ctx: CoreContext) -> Box<dyn bookmarks::Transaction> {
        STATS::update_bookmark_transaction.add_value(1);
        self.bookmarks.create_transaction(ctx, self.repoid)
    }

    // Returns the generation number of a changeset
    // note: it returns Option because changeset might not exist
    pub fn get_generation_number(
        &self,
        ctx: CoreContext,
        cs: ChangesetId,
    ) -> impl Future<Item = Option<Generation>, Error = Error> {
        STATS::get_generation_number.add_value(1);
        let repo = self.clone();
        let repoid = self.repoid.clone();
        repo.changesets
            .get(ctx, repoid, cs)
            .map(|res| res.map(|res| Generation::new(res.gen)))
    }

    pub fn get_changeset_fetcher(&self) -> Arc<dyn ChangesetFetcher> {
        (self.changeset_fetcher_factory)()
    }

    pub fn blobstore(&self) -> &RepoBlobstore {
        &self.blobstore
    }

    pub fn get_blobstore(&self) -> RepoBlobstore {
        self.blobstore.clone()
    }

    pub fn filestore_config(&self) -> FilestoreConfig {
        self.filestore_config
    }

    pub fn get_repoid(&self) -> RepositoryId {
        self.repoid
    }

    pub(crate) fn get_filenodes(&self) -> &Arc<dyn Filenodes> {
        match self.get_attribute::<dyn Filenodes>() {
            Some(attr) => attr,
            None => panic!("BlboRepo initalized incorrectly and does not have Filenodes attribute"),
        }
    }

    pub fn get_phases(&self) -> Arc<dyn Phases> {
        self.phases_factory.get_phases(
            self.repoid,
            (self.changeset_fetcher_factory)(),
            self.get_heads_fetcher(),
        )
    }

    pub fn name(&self) -> &String {
        &self.reponame
    }

    pub fn get_heads_fetcher(&self) -> HeadsFetcher {
        let this = self.clone();
        Arc::new(move |ctx: &CoreContext| {
            this.get_bonsai_heads_maybe_stale(ctx.clone())
                .collect()
                .compat()
                .boxed()
        })
    }

    /// TODO (aslpavel):
    /// This method will go away once all usages of BonsaiHgMapping will be removed
    /// from blobrepo crate. Use `BlobRepoHg::get_bonsai_hg_mapping` instead.
    /// Do not make this method public!!!
    fn get_bonsai_hg_mapping(&self) -> &Arc<dyn BonsaiHgMapping> {
        match self.get_attribute::<dyn BonsaiHgMapping>() {
            Some(attr) => attr,
            None => panic!(
                "BlboRepo initalized incorrectly and does not have BonsaiHgMapping attribute",
            ),
        }
    }

    pub fn get_bookmarks_object(&self) -> Arc<dyn Bookmarks> {
        self.bookmarks.clone()
    }

    pub fn get_phases_factory(&self) -> &SqlPhasesFactory {
        &self.phases_factory
    }

    pub fn get_changesets_object(&self) -> Arc<dyn Changesets> {
        self.changesets.clone()
    }

    pub fn get_derived_data_config(&self) -> &DerivedDataConfig {
        &self.derived_data_config
    }

    pub fn get_derived_data_lease_ops(&self) -> Arc<dyn LeaseOps> {
        self.derived_data_lease.clone()
    }
}

/// This function uploads bonsai changests object to blobstore in parallel, and then does
/// sequential writes to changesets table. Parents of the changesets should already by saved
/// in the repository.
pub fn save_bonsai_changesets(
    bonsai_changesets: Vec<BonsaiChangeset>,
    ctx: CoreContext,
    repo: BlobRepo,
) -> impl Future<Item = (), Error = Error> {
    let complete_changesets = repo.changesets.clone();
    let blobstore = repo.blobstore.clone();
    let repoid = repo.repoid.clone();

    let mut parents_to_check: HashSet<ChangesetId> = HashSet::new();
    for bcs in &bonsai_changesets {
        parents_to_check.extend(bcs.parents());
    }
    // Remove commits that we are uploading in this batch
    for bcs in &bonsai_changesets {
        parents_to_check.remove(&bcs.get_changeset_id());
    }

    let parents_to_check = stream::futures_unordered(parents_to_check.into_iter().map({
        cloned!(ctx, repo);
        move |p| {
            repo.changeset_exists_by_bonsai(ctx.clone(), p)
                .and_then(move |exists| {
                    if exists {
                        Ok(())
                    } else {
                        Err(format_err!("Commit {} does not exist in the repo", p))
                    }
                })
        }
    }))
    .collect();

    let bonsai_changesets: HashMap<_, _> = bonsai_changesets
        .into_iter()
        .map(|bcs| (bcs.get_changeset_id(), bcs))
        .collect();

    // Order of inserting bonsai changesets objects doesn't matter, so we can join them
    let mut bonsai_object_futs = FuturesUnordered::new();
    for bcs in bonsai_changesets.values() {
        bonsai_object_futs.push(save_bonsai_changeset_object(
            ctx.clone(),
            blobstore.clone(),
            bcs.clone(),
        ));
    }
    let bonsai_objects = bonsai_object_futs.collect();
    // Order of inserting entries in changeset table matters though, so we first need to
    // topologically sort commits.
    let mut bcs_parents = HashMap::new();
    for bcs in bonsai_changesets.values() {
        let parents: Vec<_> = bcs.parents().collect();
        bcs_parents.insert(bcs.get_changeset_id(), parents);
    }

    let topo_sorted_commits = sort_topological(&bcs_parents).expect("loop in commit chain!");
    let mut bonsai_complete_futs = vec![];
    for bcs_id in topo_sorted_commits {
        if let Some(bcs) = bonsai_changesets.get(&bcs_id) {
            let bcs_id = bcs.get_changeset_id();
            let completion_record = ChangesetInsert {
                repo_id: repoid,
                cs_id: bcs_id,
                parents: bcs.parents().into_iter().collect(),
            };

            bonsai_complete_futs.push(complete_changesets.add(ctx.clone(), completion_record));
        }
    }

    bonsai_objects
        .join(parents_to_check)
        .and_then(move |_| {
            loop_fn(
                bonsai_complete_futs.into_iter(),
                move |mut futs| match futs.next() {
                    Some(fut) => fut
                        .and_then(move |_| ok(Loop::Continue(futs)))
                        .left_future(),
                    None => ok(Loop::Break(())).right_future(),
                },
            )
        })
        .and_then(|_| ok(()))
}

pub fn save_bonsai_changeset_object(
    ctx: CoreContext,
    blobstore: RepoBlobstore,
    bonsai_cs: BonsaiChangeset,
) -> impl Future<Item = (), Error = Error> {
    let bonsai_blob = bonsai_cs.into_blob();
    let bcs_id = bonsai_blob.id().clone();
    let blobstore_key = bcs_id.blobstore_key();

    blobstore
        .put(ctx, blobstore_key, bonsai_blob.into())
        .map(|_| ())
}

impl Clone for BlobRepo {
    fn clone(&self) -> Self {
        Self {
            bookmarks: self.bookmarks.clone(),
            blobstore: self.blobstore.clone(),
            changesets: self.changesets.clone(),
            bonsai_git_mapping: self.bonsai_git_mapping.clone(),
            bonsai_globalrev_mapping: self.bonsai_globalrev_mapping.clone(),
            repoid: self.repoid.clone(),
            changeset_fetcher_factory: self.changeset_fetcher_factory.clone(),
            derived_data_lease: self.derived_data_lease.clone(),
            filestore_config: self.filestore_config.clone(),
            phases_factory: self.phases_factory.clone(),
            derived_data_config: self.derived_data_config.clone(),
            reponame: self.reponame.clone(),
            attributes: self.attributes.clone(),
        }
    }
}

impl DangerousOverride<Arc<dyn LeaseOps>> for BlobRepo {
    fn dangerous_override<F>(&self, modify: F) -> Self
    where
        F: FnOnce(Arc<dyn LeaseOps>) -> Arc<dyn LeaseOps>,
    {
        let derived_data_lease = modify(self.derived_data_lease.clone());
        BlobRepo {
            derived_data_lease,
            ..self.clone()
        }
    }
}

impl DangerousOverride<Arc<dyn Blobstore>> for BlobRepo {
    fn dangerous_override<F>(&self, modify: F) -> Self
    where
        F: FnOnce(Arc<dyn Blobstore>) -> Arc<dyn Blobstore>,
    {
        let (blobstore, repoid) = RepoBlobstoreArgs::new_with_wrapped_inner_blobstore(
            self.blobstore.clone(),
            self.get_repoid(),
            modify,
        )
        .into_blobrepo_parts();
        BlobRepo {
            repoid,
            blobstore,
            ..self.clone()
        }
    }
}

impl DangerousOverride<Arc<dyn Bookmarks>> for BlobRepo {
    fn dangerous_override<F>(&self, modify: F) -> Self
    where
        F: FnOnce(Arc<dyn Bookmarks>) -> Arc<dyn Bookmarks>,
    {
        let bookmarks = modify(self.bookmarks.clone());
        BlobRepo {
            bookmarks,
            ..self.clone()
        }
    }
}

impl DangerousOverride<Arc<dyn Filenodes>> for BlobRepo {
    fn dangerous_override<F>(&self, modify: F) -> Self
    where
        F: FnOnce(Arc<dyn Filenodes>) -> Arc<dyn Filenodes>,
    {
        let filenodes = modify(self.get_filenodes().clone());
        let mut attrs = self.attributes.as_ref().clone();
        attrs.insert::<dyn Filenodes>(filenodes);
        BlobRepo {
            attributes: Arc::new(attrs),
            ..self.clone()
        }
    }
}

impl DangerousOverride<Arc<dyn Changesets>> for BlobRepo {
    fn dangerous_override<F>(&self, modify: F) -> Self
    where
        F: FnOnce(Arc<dyn Changesets>) -> Arc<dyn Changesets>,
    {
        let changesets = modify(self.changesets.clone());

        let changeset_fetcher_factory = {
            cloned!(changesets, self.repoid);
            move || {
                let res: Arc<dyn ChangesetFetcher + Send + Sync> = Arc::new(
                    SimpleChangesetFetcher::new(changesets.clone(), repoid.clone()),
                );
                res
            }
        };

        BlobRepo {
            changesets,
            changeset_fetcher_factory: Arc::new(changeset_fetcher_factory),
            ..self.clone()
        }
    }
}

impl DangerousOverride<Arc<dyn BonsaiHgMapping>> for BlobRepo {
    fn dangerous_override<F>(&self, modify: F) -> Self
    where
        F: FnOnce(Arc<dyn BonsaiHgMapping>) -> Arc<dyn BonsaiHgMapping>,
    {
        let bonsai_hg_mapping = modify(self.get_bonsai_hg_mapping().clone());
        let mut attrs = self.attributes.as_ref().clone();
        attrs.insert::<dyn BonsaiHgMapping>(bonsai_hg_mapping);
        BlobRepo {
            attributes: Arc::new(attrs),
            ..self.clone()
        }
    }
}

impl DangerousOverride<DerivedDataConfig> for BlobRepo {
    fn dangerous_override<F>(&self, modify: F) -> Self
    where
        F: FnOnce(DerivedDataConfig) -> DerivedDataConfig,
    {
        let derived_data_config = modify(self.derived_data_config.clone());
        BlobRepo {
            derived_data_config,
            ..self.clone()
        }
    }
}

impl DangerousOverride<FilestoreConfig> for BlobRepo {
    fn dangerous_override<F>(&self, modify: F) -> Self
    where
        F: FnOnce(FilestoreConfig) -> FilestoreConfig,
    {
        let filestore_config = modify(self.filestore_config.clone());
        BlobRepo {
            filestore_config,
            ..self.clone()
        }
    }
}
