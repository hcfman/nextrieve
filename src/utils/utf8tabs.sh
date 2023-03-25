#!/bin/sh

# Generate UTF8 tables for nextriev.

[ ! -e UnicodeData.txt ] && echo UnicodeData.txt not found. && exit 1
[ ! -e CaseFolding.txt ] && echo CaseFolding.txt not found. && exit 1

echo "awk -F ';' -f ucd.awk < UnicodeData.txt"
awk -F ';' -f ucd.awk < UnicodeData.txt
echo "awk -F ';' -f ucf.awk < CaseFolding.txt > utf8fold.txt"
awk -F ';' -f ucf.awk < CaseFolding.txt > utf8fold.txt

# Generate code.
echo '/* Generated file */' > utf8tabs.c
echo '#include "utf8tabs.h"' >> utf8tabs.c
echo '' >> utf8tabs.c

echo 'char utf8int_class[] = {' >> utf8tabs.c
sed -e "s/./'&',/g" -e s/'$'/"'\\\\n',"/ < utf8class.txt >> utf8tabs.c
echo '0' >> utf8tabs.c
echo '};' >> utf8tabs.c
echo '' >> utf8tabs.c

echo 'char utf8int_fold[] = {' >> utf8tabs.c
sed -e "s/./'&',/g" -e s/'$'/"'\\\\n',"/ < utf8fold.txt >> utf8tabs.c
echo '0' >> utf8tabs.c
echo '};' >> utf8tabs.c
echo '' >> utf8tabs.c

echo 'char utf8int_decomp[] = {' >> utf8tabs.c
sed -e "s/./'&',/g" -e s/'$'/"'\\\\n',"/ < utf8decomp.txt >> utf8tabs.c
echo '0' >> utf8tabs.c
echo '};' >> utf8tabs.c
echo '' >> utf8tabs.c

cat > utf8tabs.h << EOF
/* Generated file */
extern char utf8int_class[];
extern char utf8int_fold[];
extern char utf8int_decomp[];
EOF

echo utf8tabs.h created.
echo utf8tabs.c created.
