SUBDIRS = include src tools tests doc bash-completion
ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}
headers = config.h

m4datadir = $(datadir)/aclocal
m4data_DATA = $(srcdir)/abigail.m4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libabigail.pc

dist_noinst_DATA = default.abignore
abigaillibdir = $(pkglibdir)
abigaillib_DATA = default.abignore

#bashcompletiondir = $(datadir)/bash-completion/completions
#dist_bashcompletion_DATA =

EXTRA_DIST = 			\
autoconf-archive/ax_check_python_modules.m4 \
autoconf-archive/ax_prog_python_version.m4 \
autoconf-archive/ax_compare_version.m4 \
NEWS README COPYING COMPILING  \
COMMIT-LOG-GUIDELINES VISIBILITY \
ChangeLog COPYING-LGPLV2 COPYING-LGPLV3	\
COPYING-GPLV3 gen-changelog.py	\
$(headers) $(m4data_DATA) 	\
libabigail.pc.in

# automake already tells which subdir is being entered.
# Don't make make repeat.
AM_MAKEFLAGS = --no-print-directory

.PHONY: doc

doc: html-doc man info

html-doc:
	$(MAKE) -C doc html-doc
	$(MAKE) -C doc/manuals html-doc

man:
	$(MAKE) -C doc/manuals man

info:
	$(MAKE) -C doc/manuals info

check-valgrind:
	$(MAKE) -C tests check-valgrind

check-valgrind-recursive:
	$(MAKE) -C tests check-valgrind-memcheck-recursive

update-changelog:
	python $(srcdir)/gen-changelog.py > $(srcdir)/ChangeLog

TARBALL = $(PACKAGE_NAME)-$(VERSION).tar.gz
RELEASED_BRANCH = master

$(TARBALL): distcheck

tag-release-only:
	git tag -m "$(PACKAGE_NAME) release $(VERSION)" \
	"$(PACKAGE_NAME)-$(VERSION)" $(RELEASED_BRANCH)

tag-release: tag-release-only

upload-release-only:
	scp $(TARBALL) sourceware.org:~ftp/pub/libabigail

tarball: $(TARBALL)

upload-release: tarball tag-release upload-release-only

release: upload-release

upload-release: tarball-and-tag-and-upload

tarball-and-tag-and-upload: tarball-and-all

tarball-and-all: tag-and-all
	$(MAKE) tarball

tag-and-all: distcheck
	$(MAKE) tag-release-only
