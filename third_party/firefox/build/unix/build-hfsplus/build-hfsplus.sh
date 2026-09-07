#!/bin/bash


set -e
set -x

hfplus_version=540.1.linux3
dirname=diskdev_cmds-${hfplus_version}
make_flags="-j$(nproc)"

root_dir="$1"
if [ -z "$root_dir" -o ! -d "$root_dir" ]; then
  root_dir=$(mktemp -d)
fi
cd $root_dir

if test -z $TMPDIR; then
  TMPDIR=/tmp/
fi

cd $dirname
patch -p1 << 'EOF'
--- a/newfs_hfs.tproj/Makefile.lnx
+++ b/newfs_hfs.tproj/Makefile.lnx
@@ -6,3 +6,3 @@
 newfs_hfs: $(OFILES)
-	${CC} ${CFLAGS} ${LDFLAGS} -o newfs_hfs ${OFILES} -lcrypto
+	${CC} ${CFLAGS} ${LDFLAGS} -o newfs_hfs ${OFILES} -Wl,-Bstatic -lcrypto -Wl,-Bdynamic,--as-needed,-lz,-ldl
 
EOF
grep -rl sysctl.h . | xargs sed -i /sysctl.h/d
make $make_flags || exit 1
cd ..

mkdir hfsplus
cp $dirname/newfs_hfs.tproj/newfs_hfs hfsplus/newfs_hfs
cp $dirname/fsck_hfs.tproj/fsck_hfs hfsplus/fsck_hfs

cd $root_dir
tar caf $root_dir/hfsplus.tar.zst hfsplus
