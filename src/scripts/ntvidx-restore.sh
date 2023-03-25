#!/bin/sh

# Move a backup index back into place as the normal index.

[ "$1" = "" ] && 1=.
cd "$1" || { echo "Cannot change directory to $1."; exit 1; }

[ ! -f idx-orig.ntv ] && { echo "No '*-orig' files in $1 to restore"; exit 1; }
if [ -f idx.ntv -a ! -f idx-opt.ntv ]
then
    # Move current index back to "opt" versions.
    for i in idx.ntv rec.ntv rfbmap?.ntv rfbmap??.ntv ref?.ntv ref??.ntv
    do
	[ ! -f "$i" ] && continue
	new=`basename "$i" .ntv`
	echo mv "$i" "$new"-opt.ntv
	mv "$i" "$new"-opt.ntv || exit 1
    done
fi

# Move backup (*-orig) files back to main index.
for i in *-orig.ntv
do
    old=`basename "$i" -orig.ntv`.ntv
    echo mv "$i" "$old"
    mv "$i" "$old" || exit 1
done

exit 0
