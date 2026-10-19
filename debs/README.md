Build output: every builder writes the .deb it produces into THIS directory
(ppa-common.sh -> DEB_DIR).  Nothing in here is committed (see ../.gitignore),
so the folder ships only as the one known place to look for a fresh artifact.
