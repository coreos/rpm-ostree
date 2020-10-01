---
nav_order: 7
---

# Releasing rpm-ostree

1. Increment the `year_version` and `release_version` macros in `configure.ac`.
2. Increment the `Version` field in `rpm-ostree.spec.in`.
3. Submit as a PR and wait until reviewed *and* CI is green.
5. Once merged, do `git pull $upstream && git reset --hard $upstream/master` on
   your local `master` branch to make sure you're on the right commit.
6. Draft release notes by seeding a HackMD.io with `git shortlog $last_tag..`
   and ideally collaborating with others. Filter out the commits from
   `dependabot`. See previous releases for format.
7. Use [`git-evtag`](https://github.com/cgwalters/git-evtag) to create a signed
   tag with the release notes as its content. Make the first line be the name of
   the tag itself.
8. Push the tag using `git push $upstream v202X.XX`.
9. Create the xz tarball using `make -C packaging -f Makefile.dist-packaging dist-snapshot`.
10. Create a GitHub release for the new release tag using its contents and
    attach the tarball.
