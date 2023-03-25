#!/bin/sh

# Use an optimized (*-opt) index.  Backup the existing index first.

[ "$1" = "" ] && 1=.
cd "$1" || { echo "Cannot change directory to $1."; exit 1; }

[ ! -f ref0-opt.ntv ] && { echo "No optimized index to use."; exit 1; }
[ ! -f ref0.ntv ] && { echo "Nothing to backup."; exit 1; }

if [ -s ref0-orig.ntv ]; then
    # Remove original backup.
    rm -f *-orig.ntv || exit 1
fi

# Backup existing index.
for i in idx.ntv rec.ntv rfbmap?.ntv rfbmap??.ntv ref?.ntv ref??.ntv
do
    [ ! -f "$i" ] && continue
    new=`basename "$i" .ntv`-orig.ntv
    echo mv "$i" "$new"
    mv "$i" "$new" || exit 1
done

# Rename optimized index files to become main index.
for i in *-opt.ntv
do
    old=`basename "$i" -opt.ntv`.ntv
    echo mv "$i" "$old"
    mv "$i" "$old" || exit 1
done

exit 0
