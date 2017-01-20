Set up test environment.
  $ . $TESTDIR/require-ext.sh directaccess evolve inhibit
  $ extpath=`dirname $TESTDIR`
  $ cp $extpath/hgext3rd/allowunstable.py $TESTTMP
  $ cp $extpath/hgext3rd/fbamend.py $TESTTMP
  $ cat >> $HGRCPATH << EOF
  > [extensions]
  > allowunstable=$TESTTMP/allowunstable.py
  > directaccess=
  > evolve=
  > fbamend=$TESTTMP/fbamend.py
  > inhibit=
  > rebase=
  > [experimental]
  > evolution = createmarkers
  > evolutioncommands = prev next fold split
  > EOF
  $ showgraph() {
  >   hg log --graph -T "{rev} {desc|firstline}" | sed \$d
  > }
  $ reset() {
  >   cd ..
  >   rm -rf repo
  >   hg init repo
  >   cd repo
  > }

Set up repo.
  $ hg init repo && cd repo
  $ hg debugbuilddag -m "+5 *4 +2"
  $ showgraph
  o  7 r7
  |
  o  6 r6
  |
  o  5 r5
  |
  | o  4 r4
  | |
  | o  3 r3
  | |
  | o  2 r2
  |/
  o  1 r1
  |
  o  0 r0

Test simple case of folding a head. Should work normally.
  $ hg up 7
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  $ hg fold --from '.^'
  2 changesets folded
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  $ showgraph
  @  8 r6
  |
  o  5 r5
  |
  | o  4 r4
  | |
  | o  3 r3
  | |
  | o  2 r2
  |/
  o  1 r1
  |
  o  0 r0

Test rebasing of stack after fold.
  $ hg up 3
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  $ hg fold --from '.^'
  2 changesets folded
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  rebasing 4:b762560d23fd "r4"
  merging mf
  $ showgraph
  o  10 r4
  |
  @  9 r2
  |
  | o  8 r6
  | |
  | o  5 r5
  |/
  o  1 r1
  |
  o  0 r0

Test rebasing of multiple children
  $ hg up 1
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  $ hg fold --from '.^'
  2 changesets folded
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  rebasing 5:* "r5" (glob)
  merging mf
  rebasing 8:* "r6" (glob)
  merging mf
  rebasing 9:* "r2" (glob)
  merging mf
  rebasing 10:* "r4" (glob)
  merging mf
  $ showgraph
  o  15 r4
  |
  o  14 r2
  |
  | o  13 r6
  | |
  | o  12 r5
  |/
  @  11 r0

Test folding multiple changesets, using default behavior of folding
up to working copy parent. Also tests situation where the branch to
rebase is not on the topmost folded commit.
  $ reset
  $ hg debugbuilddag -m "+5 *4 +2"
  $ showgraph
  o  7 r7
  |
  o  6 r6
  |
  o  5 r5
  |
  | o  4 r4
  | |
  | o  3 r3
  | |
  | o  2 r2
  |/
  o  1 r1
  |
  o  0 r0

  $ hg up 0
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  $ hg fold --from 2
  3 changesets folded
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  rebasing 3:a422badec216 "r3"
  merging mf
  rebasing 4:b762560d23fd "r4"
  merging mf
  rebasing 5:77d787dfa5b6 "r5"
  merging mf
  rebasing 6:ec6d8e65acbe "r6"
  merging mf
  rebasing 7:9c9414e0356c "r7"
  merging mf
  $ showgraph
  o  13 r7
  |
  o  12 r6
  |
  o  11 r5
  |
  | o  10 r4
  | |
  | o  9 r3
  |/
  @  8 r0

Test folding changesets unrelated to working copy parent using --exact.
Also test that using node hashes instead of rev numbers works.
  $ reset
  $ hg debugbuilddag -m +6
  $ showgraph
  o  5 r5
  |
  o  4 r4
  |
  o  3 r3
  |
  o  2 r2
  |
  o  1 r1
  |
  o  0 r0

  $ hg fold --exact 09bb8c f07e66 cb14eb
  3 changesets folded
  rebasing 4:aa70f0fe546a "r4"
  merging mf
  rebasing 5:f2987ebe5838 "r5"
  merging mf
  $ showgraph
  o  8 r5
  |
  o  7 r4
  |
  o  6 r1
  |
  o  0 r0

Test --norebase flag.
  $ hg fold --norebase --exact 6 7
  2 changesets folded
  $ showgraph
  o  9 r1
  |
  | o  8 r5
  | |
  | o  7 r4
  | |
  | o  6 r1
  |/
  o  0 r0

Test case in which inhibit fails to inhibit the working copy parent, but
does inhibit its descendants in the old stack. The fold command should
manually inhibit any visible obsolete commits in the old stack.
  $ hg up 7
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  $ hg fold --from 8
  2 changesets folded
  1 files updated, 0 files merged, 0 files removed, 0 files unresolved
  $ showgraph
  @  10 r4
  |
  | o  9 r1
  | |
  o |  6 r1
  |/
  o  0 r0
