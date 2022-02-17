%undefine _package_note_flags
%global _hardened_build 1

%ifarch %{kernel_arches}
# ppc64le broken in rawhide:
# https://bugzilla.redhat.com/show_bug.cgi?id=2006709
# riscv64 tests fail with
# qemu-system-riscv64: invalid accelerator kvm
# qemu-system-riscv64: falling back to tcg
# qemu-system-riscv64: unable to find CPU model 'host'
# This seems to require changes in libguestfs and/or qemu to support
# -cpu max or -cpu virt.
# s390x builders can't run libguestfs
%ifnarch %{power64} riscv64 s390 s390x
%global have_libguestfs 1
%endif
%endif

# We can only compile the OCaml plugin on platforms which have native
# OCaml support (not bytecode).
%ifarch %{ocaml_native_compiler}
%global have_ocaml 1
%endif

# Architectures where we run the complete test suite including
# the libguestfs tests.
#
# On all other architectures, a simpler test suite must pass.  This
# omits any tests that run full qemu, since running qemu under TCG is
# often broken on non-x86_64 arches.
%global complete_test_arches x86_64

# If the test suite is broken on a particular architecture, document
# it as a bug and add it to this list.
%global broken_test_arches NONE

%if 0%{?rhel} == 7
# On RHEL 7, nothing in the virt stack is shipped on aarch64 and
# libguestfs was not shipped on POWER (fixed in 7.5).  We could in
# theory make all of this work by having lots more conditionals, but
# for now limit this package to x86_64 on RHEL.
ExclusiveArch:  x86_64
%endif

# If we should verify tarball signature with GPGv2.
%global verify_tarball_signature 1

# If there are patches which touch autotools files, set this to 1.
%global patches_touch_autotools %{nil}

# The source directory.
%global source_directory 1.29-development

Name:           nbdkit
Epoch: 100
Version:        1.29.15
Release:        1%{?dist}
Summary:        NBD server

License:        BSD
URL:            https://gitlab.com/nbdkit/nbdkit

%if 0%{?rhel} >= 8
# On RHEL 8+, we cannot build the package on i686 (no virt stack).
ExcludeArch:    i686
%endif

Source0:        http://libguestfs.org/download/nbdkit/%{source_directory}/%{name}-%{version}.tar.gz
%if 0%{verify_tarball_signature}
Source1:        http://libguestfs.org/download/nbdkit/%{source_directory}/%{name}-%{version}.tar.gz.sig
# Keyring used to verify tarball signature.
Source2:        libguestfs.keyring
%endif

# Maintainer script which helps with handling patches.
Source3:        copy-patches.sh

BuildRequires: make
%if 0%{patches_touch_autotools}
BuildRequires:  autoconf, automake, libtool
%endif

BuildRequires:  gcc, gcc-c++
BuildRequires:  gnutls-devel
BuildRequires:  libselinux-devel
%if !0%{?rhel} && 0%{?have_libguestfs}
BuildRequires:  libguestfs-devel
%endif
BuildRequires:  libvirt-devel
BuildRequires:  xz-devel
BuildRequires:  zlib-devel
BuildRequires:  libzstd-devel
BuildRequires:  libcurl-devel
BuildRequires:  libnbd-devel >= 1.3.11
BuildRequires:  libssh-devel
BuildRequires:  e2fsprogs, e2fsprogs-devel
%if !0%{?rhel}
BuildRequires:  xorriso
%endif
BuildRequires:  bash-completion
BuildRequires:  perl-devel
BuildRequires:  perl(ExtUtils::Embed)
%if 0%{?rhel} == 8
BuildRequires:  platform-python-devel
%else
BuildRequires:  python3-devel
%endif
%if !0%{?rhel}
BuildRequires:  python3-boto3
%endif
%if !0%{?rhel}
%if 0%{?have_ocaml}
BuildRequires:  ocaml >= 4.03
BuildRequires:  ocaml-ocamldoc
%endif
BuildRequires:  ruby-devel
BuildRequires:  lua-devel
%endif
BuildRequires: fuse3

# nbdkit is a metapackage pulling the server and a useful subset
# of the plugins and filters.
Requires:       nbdkit-server%{?_isa} = %{version}-%{release}
Requires:       nbdkit-basic-plugins%{?_isa} = %{version}-%{release}
Requires:       nbdkit-basic-filters%{?_isa} = %{version}-%{release}


%description
NBD is a protocol for accessing block devices (hard disks and
disk-like things) over the network.

nbdkit is a toolkit for creating NBD servers.

The key features are:

* Multithreaded NBD server written in C with good performance.

* Minimal dependencies for the basic server.

* Liberal license (BSD) allows nbdkit to be linked to proprietary
  libraries or included in proprietary code.

* Well-documented, simple plugin API with a stable ABI guarantee.
  Lets you to export "unconventional" block devices easily.

* You can write plugins in C or many other languages.

* Filters can be stacked in front of plugins to transform the output.

'%{name}' is a meta-package which pulls in the core server and a
useful subset of plugins and filters with minimal dependencies.

If you want just the server, install '%{name}-server'.

To develop plugins, install the '%{name}-devel' package and start by
reading the nbdkit(1) and nbdkit-plugin(3) manual pages.


%package server
Summary:        The %{name} server
License:        BSD
Provides:       %{name}-null-plugin = %{version}-%{release}

%description server
This package contains the %{name} server with only the null plugin
and no filters.  To install a basic set of plugins and filters you
need to install "nbdkit-basic-plugins", "nbdkit-basic-filters" or
the metapackage "nbdkit".


%package basic-plugins
Summary:        Basic plugins for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
Provides:       %{name}-data-plugin = %{version}-%{release}
Provides:       %{name}-eval-plugin = %{version}-%{release}
Provides:       %{name}-file-plugin = %{version}-%{release}
Provides:       %{name}-floppy-plugin = %{version}-%{release}
Provides:       %{name}-full-plugin = %{version}-%{release}
Provides:       %{name}-info-plugin = %{version}-%{release}
Provides:       %{name}-memory-plugin = %{version}-%{release}
Provides:       %{name}-ondemand-plugin = %{version}-%{release}
Provides:       %{name}-pattern-plugin = %{version}-%{release}
Provides:       %{name}-partitioning-plugin = %{version}-%{release}
Provides:       %{name}-random-plugin = %{version}-%{release}
Provides:       %{name}-sh-plugin = %{version}-%{release}
Provides:       %{name}-sparse-random-plugin = %{version}-%{release}
Provides:       %{name}-split-plugin = %{version}-%{release}
Provides:       %{name}-zero-plugin = %{version}-%{release}


%description basic-plugins
This package contains plugins for %{name} which only depend on simple
C libraries: glibc, gnutls, libzstd.  Other plugins for nbdkit with
more complex dependencies are packaged separately.

nbdkit-data-plugin          Serve small amounts of data from the command line.

nbdkit-eval-plugin          Write a shell script plugin on the command line.

nbdkit-file-plugin          The normal file plugin for serving files.

nbdkit-floppy-plugin        Create a virtual floppy disk from a directory.

nbdkit-full-plugin          A virtual disk that returns ENOSPC errors.

nbdkit-info-plugin          Serve client and server information.

nbdkit-memory-plugin        A virtual memory plugin.

nbdkit-ondemand-plugin      Create filesystems on demand.

nbdkit-pattern-plugin       Fixed test pattern.

nbdkit-partitioning-plugin  Create virtual disks from partitions.

nbdkit-random-plugin        Random content plugin for testing.

nbdkit-sh-plugin            Write plugins as shell scripts or executables.

nbdkit-sparse-random-plugin Make sparse random disks.

nbdkit-split-plugin         Concatenate one or more files.

nbdkit-zero-plugin          Zero-length plugin for testing.


%package example-plugins
Summary:        Example plugins for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
%if !0%{?rhel}
# example4 is written in Perl.
Requires:       %{name}-perl-plugin
%endif

%description example-plugins
This package contains example plugins for %{name}.


# The plugins below have non-trivial dependencies are so are
# packaged separately.

%if !0%{?rhel}
%package cc-plugin
Summary:        Write small inline C plugins and scripts for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
Requires:       gcc
Requires:       %{_bindir}/cat

%description cc-plugin
This package contains support for writing inline C plugins and scripts
for %{name}.  NOTE this is NOT the right package for writing plugins
in C, install %{name}-devel for that.
%endif


%if !0%{?rhel}
%package cdi-plugin
Summary:        Containerized Data Import plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
Requires:       jq
Requires:       podman

%description cdi-plugin
This package contains Containerized Data Import support for %{name}.
%endif


%package curl-plugin
Summary:        HTTP/FTP (cURL) plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description curl-plugin
This package contains cURL (HTTP/FTP) support for %{name}.


%if !0%{?rhel} && 0%{?have_libguestfs}
%package guestfs-plugin
Summary:        libguestfs plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description guestfs-plugin
This package is a libguestfs plugin for %{name}.
%endif


%if !0%{?rhel}
%package iso-plugin
Summary:        Virtual ISO 9660 plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
Requires:       xorriso

%description iso-plugin
This package is a virtual ISO 9660 (CD-ROM) plugin for %{name}.
%endif


%if !0%{?rhel}
%package libvirt-plugin
Summary:        Libvirt plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description libvirt-plugin
This package is a libvirt plugin for %{name}.  It lets you access
libvirt guest disks readonly.  It is implemented using the libvirt
virDomainBlockPeek API.
%endif


%package linuxdisk-plugin
Summary:        Virtual Linux disk plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
# for mke2fs
Requires:       e2fsprogs

%description linuxdisk-plugin
This package is a virtual Linux disk plugin for %{name}.


%if !0%{?rhel}
%package lua-plugin
Summary:        Lua plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description lua-plugin
This package lets you write Lua plugins for %{name}.
%endif


%package nbd-plugin
Summary:        NBD proxy / forward plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description nbd-plugin
This package lets you forward NBD connections from %{name}
to another NBD server.


%if !0%{?rhel} && 0%{?have_ocaml}
%package ocaml-plugin
Summary:        OCaml plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description ocaml-plugin
This package lets you run OCaml plugins for %{name}.

To compile OCaml plugins you will also need to install
%{name}-ocaml-plugin-devel.


%package ocaml-plugin-devel
Summary:        OCaml development environment for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
Requires:       %{name}-ocaml-plugin%{?_isa} = %{version}-%{release}

%description ocaml-plugin-devel
This package lets you write OCaml plugins for %{name}.
%endif


%if !0%{?rhel}
%package perl-plugin
Summary:        Perl plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description perl-plugin
This package lets you write Perl plugins for %{name}.
%endif


%package python-plugin
Summary:        Python 3 plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description python-plugin
This package lets you write Python 3 plugins for %{name}.


%if !0%{?rhel}
%package ruby-plugin
Summary:        Ruby plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description ruby-plugin
This package lets you write Ruby plugins for %{name}.
%endif


%if !0%{?rhel}
# In theory this is noarch, but because plugins are placed in _libdir
# which varies across architectures, RPM does not allow this.
%package S3-plugin
Summary:        Amazon S3 and Ceph plugin for %{name}
License:        BSD
Requires:       %{name}-python-plugin >= 1.22
# XXX Should not need to add this.
Requires:       python3-boto3

%description S3-plugin
This package lets you open disk images stored in Amazon S3
or Ceph using %{name}.
%endif


%package ssh-plugin
Summary:        SSH plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description ssh-plugin
This package contains SSH support for %{name}.



%package tmpdisk-plugin
Summary:        Remote temporary filesystem disk plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
# For mkfs and mke2fs (defaults).
Requires:       util-linux, e2fsprogs
# For other filesystems.
Suggests:       xfsprogs
%if !0%{?rhel}
Suggests:       ntfsprogs, dosfstools
%endif

%description tmpdisk-plugin
This package is a remote temporary filesystem disk plugin for %{name}.



%ifarch x86_64
%package vddk-plugin
Summary:        VMware VDDK plugin for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
# https://bugzilla.redhat.com/show_bug.cgi?id=1931818
Requires:       libxcrypt-compat

%description vddk-plugin
This package is a plugin for %{name} which connects to
VMware VDDK for accessing VMware disks and servers.
%endif


%package basic-filters
Summary:        Basic filters for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
Provides:       %{name}-blocksize-filter = %{version}-%{release}
Provides:       %{name}-cache-filter = %{version}-%{release}
Provides:       %{name}-cacheextents-filter = %{version}-%{release}
Provides:       %{name}-checkwrite-filter = %{version}-%{release}
Provides:       %{name}-cow-filter = %{version}-%{release}
Provides:       %{name}-ddrescue-filter = %{version}-%{release}
Provides:       %{name}-delay-filter = %{version}-%{release}
Provides:       %{name}-error-filter = %{version}-%{release}
Provides:       %{name}-exitlast-filter = %{version}-%{release}
Provides:       %{name}-exitwhen-filter = %{version}-%{release}
Provides:       %{name}-exportname-filter = %{version}-%{release}
Provides:       %{name}-extentlist-filter = %{version}-%{release}
Provides:       %{name}-fua-filter = %{version}-%{release}
Provides:       %{name}-ip-filter = %{version}-%{release}
Provides:       %{name}-limit-filter = %{version}-%{release}
Provides:       %{name}-log-filter = %{version}-%{release}
Provides:       %{name}-multi-conn-filter = %{version}-%{release}
Provides:       %{name}-nocache-filter = %{version}-%{release}
Provides:       %{name}-noextents-filter = %{version}-%{release}
Provides:       %{name}-nofilter-filter = %{version}-%{release}
Provides:       %{name}-noparallel-filter = %{version}-%{release}
Provides:       %{name}-nozero-filter = %{version}-%{release}
Provides:       %{name}-offset-filter = %{version}-%{release}
Provides:       %{name}-partition-filter = %{version}-%{release}
Provides:       %{name}-pause-filter = %{version}-%{release}
Provides:       %{name}-protect-filter = %{version}-%{release}
Provides:       %{name}-rate-filter = %{version}-%{release}
Provides:       %{name}-readahead-filter = %{version}-%{release}
Provides:       %{name}-retry-filter = %{version}-%{release}
Provides:       %{name}-retry-request-filter = %{version}-%{release}
Provides:       %{name}-stats-filter = %{version}-%{release}
Provides:       %{name}-swab-filter = %{version}-%{release}
Provides:       %{name}-tls-fallback-filter = %{version}-%{release}
Provides:       %{name}-truncate-filter = %{version}-%{release}

%description basic-filters
This package contains filters for %{name} which only depend on simple
C libraries: glibc, gnutls.  Other filters for nbdkit with more
complex dependencies are packaged separately.

nbdkit-blocksize-filter    Adjust block size of requests sent to plugins.

nbdkit-cache-filter        Server-side cache.

nbdkit-cacheextents-filter Cache extents.

nbdkit-checkwrite-filter   Check writes match contents of plugin.

nbdkit-cow-filter          Copy-on-write overlay for read-only plugins.

nbdkit-ddrescue-filter     Filter for serving from ddrescue dump.

nbdkit-delay-filter        Inject read and write delays.

nbdkit-error-filter        Inject errors.

nbdkit-exitlast-filter     Exit on last client connection.

nbdkit-exitwhen-filter     Exit gracefully when an event occurs.

nbdkit-exportname-filter   Adjust export names between client and plugin.

nbdkit-extentlist-filter   Place extent list over a plugin.

nbdkit-fua-filter          Modify flush behaviour in plugins.

nbdkit-ip-filter           Filter clients by IP address.

nbdkit-limit-filter        Limit nr clients that can connect concurrently.

nbdkit-log-filter          Log all transactions to a file.

nbdkit-multi-conn-filter   Enable, emulate or disable multi-conn.

nbdkit-nocache-filter      Disable cache requests in the underlying plugin.

nbdkit-noextents-filter    Disable extents in the underlying plugin.

nbdkit-nofilter-filter     Passthrough filter.

nbdkit-noparallel-filter   Serialize requests to the underlying plugin.

nbdkit-nozero-filter       Adjust handling of zero requests by plugins.

nbdkit-offset-filter       Serve an offset and range.

nbdkit-partition-filter    Serve a single partition.

nbdkit-pause-filter        Pause NBD requests.

nbdkit-protect-filter      Write-protect parts of a plugin.

nbdkit-rate-filter         Limit bandwidth by connection or server.

nbdkit-readahead-filter    Prefetch data when reading sequentially.

nbdkit-retry-filter        Reopen connection on error.

nbdkit-retry--request-filter Retry single requests on error.

nbdkit-stats-filter        Display statistics about operations.

nbdkit-swab-filter         Filter for swapping byte order.

nbdkit-tls-fallback-filter TLS protection filter.

nbdkit-truncate-filter     Truncate, expand, round up or round down size.


%if !0%{?rhel}
%package ext2-filter
Summary:        ext2, ext3 and ext4 filesystem support for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description ext2-filter
This package contains ext2, ext3 and ext4 filesystem support for
%{name}.
%endif


%package gzip-filter
Summary:        GZip filter for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description gzip-filter
This package is a gzip filter for %{name}.


%package tar-filter
Summary:        Tar archive filter for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
Requires:       tar
Obsoletes:      %{name}-tar-plugin < 1.23.9-3

%description tar-filter
This package is a tar archive filter for %{name}.


%package xz-filter
Summary:        XZ filter for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}

%description xz-filter
This package is the xz filter for %{name}.


%package devel
Summary:        Development files and documentation for %{name}
License:        BSD
Requires:       %{name}-server%{?_isa} = %{version}-%{release}
Requires:       pkgconfig

%description devel
This package contains development files and documentation
for %{name}.  Install this package if you want to develop
plugins for %{name}.


%package bash-completion
Summary:       Bash tab-completion for %{name}
BuildArch:     noarch
Requires:      bash-completion >= 2.0
Requires:      %{name}-server = %{version}-%{release}

%description bash-completion
Install this package if you want intelligent bash tab-completion
for %{name}.


%prep
%if 0%{verify_tarball_signature}
%{gpgverify} --keyring='%{SOURCE2}' --signature='%{SOURCE1}' --data='%{SOURCE0}'
%endif
%autosetup -p1
%if 0%{patches_touch_autotools}
autoreconf -i
%endif


%build
# Golang bindings are not enabled in the build since they don't
# need to be.  Most people would use them by copying the upstream
# package into their vendor/ directory.
export PYTHON=%{__python3}
%configure \
    --with-extra='%{name}-%{version}-%{release}' \
    --disable-static \
    --disable-golang \
    --disable-rust \
%if !0%{?rhel} && 0%{?have_ocaml}
    --enable-ocaml \
%else
    --disable-ocaml \
%endif
%if 0%{?rhel}
    --disable-lua \
    --disable-perl \
    --disable-ruby \
    --disable-tcl \
    --without-ext2 \
    --without-iso \
    --without-libvirt \
%endif
%if !0%{?rhel} && 0%{?have_libguestfs}
    --with-libguestfs \
%else
    --without-libguestfs \
%endif
%ifarch %{complete_test_arches}
    --enable-libguestfs-tests \
%else
    --disable-libguestfs-tests \
%endif
    --with-tls-priority=@NBDKIT,SYSTEM

# Verify that it picked the correct version of Python
# to avoid RHBZ#1404631 happening again silently.
grep '^PYTHON_VERSION = 3' Makefile

%make_build


%install
%make_install

# Delete libtool crap.
find $RPM_BUILD_ROOT -name '*.la' -delete

# If cargo happens to be installed on the machine then the
# rust plugin is built.  Delete it if this happens.
rm -f $RPM_BUILD_ROOT%{_mandir}/man3/nbdkit-rust-plugin.3*

%if 0%{?rhel}
# In RHEL, remove some plugins we cannot --disable.
for f in cc cdi torrent; do
    rm -f $RPM_BUILD_ROOT%{_libdir}/%{name}/plugins/nbdkit-$f-plugin.so
    rm -f $RPM_BUILD_ROOT%{_mandir}/man?/nbdkit-$f-plugin.*
done
rm -f $RPM_BUILD_ROOT%{_libdir}/%{name}/plugins/nbdkit-S3-plugin
rm -f $RPM_BUILD_ROOT%{_mandir}/man1/nbdkit-S3-plugin.1*
%endif


%check
%ifnarch %{broken_test_arches}
function skip_test ()
{
    for f in "$@"; do
        rm -f "$f"
        echo 'exit 77' > "$f"
        chmod +x "$f"
    done
}

# Workaround for broken libvirt (RHBZ#1138604).
mkdir -p $HOME/.cache/libvirt

# tests/test-captive.sh is racy especially on s390x.  We need to
# rethink this test upstream.
skip_test tests/test-captive.sh

%ifarch s390x
# Temporarily kill tests/test-cache-max-size.sh since it fails
# sometimes on s390x for unclear reasons.
skip_test tests/test-cache-max-size.sh
%endif

# Temporarily kill test-nbd-tls.sh and test-nbd-tls-psk.sh
# https://www.redhat.com/archives/libguestfs/2020-March/msg00191.html
skip_test tests/test-nbd-tls.sh tests/test-nbd-tls-psk.sh

# This test fails on RHEL 9 aarch64 & ppc64le with the error:
# nbdkit: error: allocator=malloc: mlock: Cannot allocate memory
# It could be the mlock limit on the builder is too low.
# https://bugzilla.redhat.com/show_bug.cgi?id=2044432
%if 0%{?rhel}
%ifarch aarch64 %{power64}
skip_test tests/test-memory-allocator-malloc-mlock.sh
%endif
%endif

# armv7 is very slow and some tests take an especially long time to
# run.  This skips some of the slowest tests.  Note that
# test-data-format.sh is a slow test, but in the past we have found
# bugs by running it on armv7.
%ifarch %{arm}
skip_test tests/test-partition1.sh tests/test-cow.sh tests/test-cow-block-size.sh tests/test-cow-extents2.sh
%endif

# Make sure we can see the debug messages (RHBZ#1230160).
export LIBGUESTFS_DEBUG=1
export LIBGUESTFS_TRACE=1

%make_build check || {
    cat tests/test-suite.log
    exit 1
  }
%endif


%if 0%{?have_ocaml}
%ldconfig_scriptlets plugin-ocaml
%endif


%files
# metapackage so empty


%files server
%doc README
%license LICENSE
%{_sbindir}/nbdkit
%dir %{_libdir}/%{name}
%dir %{_libdir}/%{name}/plugins
%{_libdir}/%{name}/plugins/nbdkit-null-plugin.so
%dir %{_libdir}/%{name}/filters
%{_mandir}/man1/nbdkit.1*
%{_mandir}/man1/nbdkit-captive.1*
%{_mandir}/man1/nbdkit-client.1*
%{_mandir}/man1/nbdkit-loop.1*
%{_mandir}/man1/nbdkit-null-plugin.1*
%{_mandir}/man1/nbdkit-probing.1*
%{_mandir}/man1/nbdkit-protocol.1*
%{_mandir}/man1/nbdkit-service.1*
%{_mandir}/man1/nbdkit-security.1*
%{_mandir}/man1/nbdkit-tls.1*


%files basic-plugins
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-data-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-eval-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-file-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-floppy-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-full-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-info-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-memory-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-ondemand-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-partitioning-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-pattern-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-random-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-sh-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-sparse-random-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-split-plugin.so
%{_libdir}/%{name}/plugins/nbdkit-zero-plugin.so
%{_mandir}/man1/nbdkit-data-plugin.1*
%{_mandir}/man1/nbdkit-eval-plugin.1*
%{_mandir}/man1/nbdkit-file-plugin.1*
%{_mandir}/man1/nbdkit-floppy-plugin.1*
%{_mandir}/man1/nbdkit-full-plugin.1*
%{_mandir}/man1/nbdkit-info-plugin.1*
%{_mandir}/man1/nbdkit-memory-plugin.1*
%{_mandir}/man1/nbdkit-ondemand-plugin.1*
%{_mandir}/man1/nbdkit-partitioning-plugin.1*
%{_mandir}/man1/nbdkit-pattern-plugin.1*
%{_mandir}/man1/nbdkit-random-plugin.1*
%{_mandir}/man3/nbdkit-sh-plugin.3*
%{_mandir}/man1/nbdkit-sparse-random-plugin.1*
%{_mandir}/man1/nbdkit-split-plugin.1*
%{_mandir}/man1/nbdkit-zero-plugin.1*


%files example-plugins
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-example*-plugin.so
%if !0%{?rhel}
%{_libdir}/%{name}/plugins/nbdkit-example4-plugin
%endif
%{_mandir}/man1/nbdkit-example*-plugin.1*


%if !0%{?rhel}
%files cc-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-cc-plugin.so
%{_mandir}/man3/nbdkit-cc-plugin.3*
%endif


%if !0%{?rhel}
%files cdi-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-cdi-plugin.so
%{_mandir}/man1/nbdkit-cdi-plugin.1*
%endif


%files curl-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-curl-plugin.so
%{_mandir}/man1/nbdkit-curl-plugin.1*


%if !0%{?rhel} && 0%{?have_libguestfs}
%files guestfs-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-guestfs-plugin.so
%{_mandir}/man1/nbdkit-guestfs-plugin.1*
%endif


%if !0%{?rhel}
%files iso-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-iso-plugin.so
%{_mandir}/man1/nbdkit-iso-plugin.1*
%endif


%if !0%{?rhel}
%files libvirt-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-libvirt-plugin.so
%{_mandir}/man1/nbdkit-libvirt-plugin.1*
%endif


%files linuxdisk-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-linuxdisk-plugin.so
%{_mandir}/man1/nbdkit-linuxdisk-plugin.1*


%if !0%{?rhel}
%files lua-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-lua-plugin.so
%{_mandir}/man3/nbdkit-lua-plugin.3*
%endif


%files nbd-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-nbd-plugin.so
%{_mandir}/man1/nbdkit-nbd-plugin.1*


%if !0%{?rhel} && 0%{?have_ocaml}
%files ocaml-plugin
%doc README
%license LICENSE
%{_libdir}/libnbdkitocaml.so.*

%files ocaml-plugin-devel
%{_libdir}/libnbdkitocaml.so
%{_libdir}/ocaml/NBDKit.*
%{_mandir}/man3/nbdkit-ocaml-plugin.3*
%{_mandir}/man3/NBDKit.3*
%endif


%if !0%{?rhel}
%files perl-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-perl-plugin.so
%{_mandir}/man3/nbdkit-perl-plugin.3*
%endif


%files python-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-python-plugin.so
%{_mandir}/man3/nbdkit-python-plugin.3*


%if !0%{?rhel}
%files ruby-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-ruby-plugin.so
%{_mandir}/man3/nbdkit-ruby-plugin.3*
%endif


%if !0%{?rhel}
%files S3-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-S3-plugin
%{_mandir}/man1/nbdkit-S3-plugin.1*
%endif


%files ssh-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-ssh-plugin.so
%{_mandir}/man1/nbdkit-ssh-plugin.1*



%files tmpdisk-plugin
%doc README
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-tmpdisk-plugin.so
%{_mandir}/man1/nbdkit-tmpdisk-plugin.1*



%ifarch x86_64
%files vddk-plugin
%doc README plugins/vddk/README.VDDK
%license LICENSE
%{_libdir}/%{name}/plugins/nbdkit-vddk-plugin.so
%{_mandir}/man1/nbdkit-vddk-plugin.1*
%endif


%files basic-filters
%doc README
%license LICENSE
%{_libdir}/%{name}/filters/nbdkit-blocksize-filter.so
%{_libdir}/%{name}/filters/nbdkit-cache-filter.so
%{_libdir}/%{name}/filters/nbdkit-cacheextents-filter.so
%{_libdir}/%{name}/filters/nbdkit-checkwrite-filter.so
%{_libdir}/%{name}/filters/nbdkit-cow-filter.so
%{_libdir}/%{name}/filters/nbdkit-ddrescue-filter.so
%{_libdir}/%{name}/filters/nbdkit-delay-filter.so
%{_libdir}/%{name}/filters/nbdkit-error-filter.so
%{_libdir}/%{name}/filters/nbdkit-exitlast-filter.so
%{_libdir}/%{name}/filters/nbdkit-exitwhen-filter.so
%{_libdir}/%{name}/filters/nbdkit-exportname-filter.so
%{_libdir}/%{name}/filters/nbdkit-extentlist-filter.so
%{_libdir}/%{name}/filters/nbdkit-fua-filter.so
%{_libdir}/%{name}/filters/nbdkit-ip-filter.so
%{_libdir}/%{name}/filters/nbdkit-limit-filter.so
%{_libdir}/%{name}/filters/nbdkit-log-filter.so
%{_libdir}/%{name}/filters/nbdkit-multi-conn-filter.so
%{_libdir}/%{name}/filters/nbdkit-nocache-filter.so
%{_libdir}/%{name}/filters/nbdkit-noextents-filter.so
%{_libdir}/%{name}/filters/nbdkit-nofilter-filter.so
%{_libdir}/%{name}/filters/nbdkit-noparallel-filter.so
%{_libdir}/%{name}/filters/nbdkit-nozero-filter.so
%{_libdir}/%{name}/filters/nbdkit-offset-filter.so
%{_libdir}/%{name}/filters/nbdkit-partition-filter.so
%{_libdir}/%{name}/filters/nbdkit-pause-filter.so
%{_libdir}/%{name}/filters/nbdkit-protect-filter.so
%{_libdir}/%{name}/filters/nbdkit-rate-filter.so
%{_libdir}/%{name}/filters/nbdkit-readahead-filter.so
%{_libdir}/%{name}/filters/nbdkit-retry-filter.so
%{_libdir}/%{name}/filters/nbdkit-retry-request-filter.so
%{_libdir}/%{name}/filters/nbdkit-stats-filter.so
%{_libdir}/%{name}/filters/nbdkit-swab-filter.so
%{_libdir}/%{name}/filters/nbdkit-tls-fallback-filter.so
%{_libdir}/%{name}/filters/nbdkit-truncate-filter.so
%{_mandir}/man1/nbdkit-blocksize-filter.1*
%{_mandir}/man1/nbdkit-cache-filter.1*
%{_mandir}/man1/nbdkit-cacheextents-filter.1*
%{_mandir}/man1/nbdkit-checkwrite-filter.1*
%{_mandir}/man1/nbdkit-cow-filter.1*
%{_mandir}/man1/nbdkit-ddrescue-filter.1*
%{_mandir}/man1/nbdkit-delay-filter.1*
%{_mandir}/man1/nbdkit-error-filter.1*
%{_mandir}/man1/nbdkit-exitlast-filter.1*
%{_mandir}/man1/nbdkit-exitwhen-filter.1*
%{_mandir}/man1/nbdkit-exportname-filter.1*
%{_mandir}/man1/nbdkit-extentlist-filter.1*
%{_mandir}/man1/nbdkit-fua-filter.1*
%{_mandir}/man1/nbdkit-ip-filter.1*
%{_mandir}/man1/nbdkit-limit-filter.1*
%{_mandir}/man1/nbdkit-log-filter.1*
%{_mandir}/man1/nbdkit-multi-conn-filter.1*
%{_mandir}/man1/nbdkit-nocache-filter.1*
%{_mandir}/man1/nbdkit-noextents-filter.1*
%{_mandir}/man1/nbdkit-nofilter-filter.1*
%{_mandir}/man1/nbdkit-noparallel-filter.1*
%{_mandir}/man1/nbdkit-nozero-filter.1*
%{_mandir}/man1/nbdkit-offset-filter.1*
%{_mandir}/man1/nbdkit-partition-filter.1*
%{_mandir}/man1/nbdkit-pause-filter.1*
%{_mandir}/man1/nbdkit-protect-filter.1*
%{_mandir}/man1/nbdkit-rate-filter.1*
%{_mandir}/man1/nbdkit-readahead-filter.1*
%{_mandir}/man1/nbdkit-retry-filter.1*
%{_mandir}/man1/nbdkit-retry-request-filter.1*
%{_mandir}/man1/nbdkit-stats-filter.1*
%{_mandir}/man1/nbdkit-swab-filter.1*
%{_mandir}/man1/nbdkit-tls-fallback-filter.1*
%{_mandir}/man1/nbdkit-truncate-filter.1*


%if !0%{?rhel}
%files ext2-filter
%doc README
%license LICENSE
%{_libdir}/%{name}/filters/nbdkit-ext2-filter.so
%{_mandir}/man1/nbdkit-ext2-filter.1*
%endif


%files gzip-filter
%doc README
%license LICENSE
%{_libdir}/%{name}/filters/nbdkit-gzip-filter.so
%{_mandir}/man1/nbdkit-gzip-filter.1*


%files tar-filter
%doc README
%license LICENSE
%{_libdir}/%{name}/filters/nbdkit-tar-filter.so
%{_mandir}/man1/nbdkit-tar-filter.1*


%files xz-filter
%doc README
%license LICENSE
%{_libdir}/%{name}/filters/nbdkit-xz-filter.so
%{_mandir}/man1/nbdkit-xz-filter.1*


%files devel
%doc BENCHMARKING OTHER_PLUGINS README SECURITY TODO
%license LICENSE
# Include the source of the example plugins in the documentation.
%doc plugins/example*/*.c
%if !0%{?rhel}
%doc plugins/example4/nbdkit-example4-plugin
%doc plugins/lua/example.lua
%endif
%if !0%{?rhel} && 0%{?have_ocaml}
%doc plugins/ocaml/example.ml
%endif
%if !0%{?rhel}
%doc plugins/perl/example.pl
%endif
%doc plugins/python/examples/*.py
%if !0%{?rhel}
%doc plugins/ruby/example.rb
%endif
%doc plugins/sh/example.sh
%{_includedir}/nbdkit-common.h
%{_includedir}/nbdkit-filter.h
%{_includedir}/nbdkit-plugin.h
%{_includedir}/nbdkit-version.h
%{_includedir}/nbd-protocol.h
%{_mandir}/man3/nbdkit-filter.3*
%{_mandir}/man3/nbdkit-plugin.3*
%{_mandir}/man1/nbdkit-release-notes-1.*.1*
%{_libdir}/pkgconfig/nbdkit.pc


%files bash-completion
%license LICENSE
%dir %{_datadir}/bash-completion/completions
%{_datadir}/bash-completion/completions/nbdkit


%changelog
