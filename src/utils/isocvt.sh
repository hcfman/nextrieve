#!/bin/sh

#
# Convert the content of an iso*.txt file to C-arrays.
#
#
# =3E	U+003E	GREATER-THAN SIGN

simpname=`basename "$1" .txt | tr -dc '[:alnum:]'`

[ -z "$1" ] \
    && {
	    echo 'Usage: iso-xxx.txt.'
	    exit 1
       }

sed -e 's/^=\(..\).U+\(....\).*/\1 \2/' \
    < "$1" \
    | awk -v simpname=$simpname 'BEGIN {
	    hex["0"] = 0;
	    hex["1"] = 1;
	    hex["2"] = 2;
	    hex["3"] = 3;
	    hex["4"] = 4;
	    hex["5"] = 5;
	    hex["6"] = 6;
	    hex["7"] = 7;
	    hex["8"] = 8;
	    hex["9"] = 9;
	    hex["A"] = 10;
	    hex["B"] = 11;
	    hex["C"] = 12;
	    hex["D"] = 13;
	    hex["E"] = 14;
	    hex["F"] = 15;
           }
	   {
	    code=$1;
	    ucode=$2;
	    chars[hex[substr(code, 1, 1)]*16+hex[substr(code, 2, 1)]] =\
		hex[substr(ucode, 1, 1)]*16*16*16 \
		+hex[substr(ucode, 2, 1)]*16*16 \
		+hex[substr(ucode, 3, 1)]*16 \
		+hex[substr(ucode, 4, 1)];
	    }
	   END {printf("unsigned char *%schars_str[256] = {\n", simpname);
		i = 0;
		for (row = 0; row < 32; row++)
		{
		    for (col = 0; col < 8; col++)
		    {
			if (chars[i] == 0)
			    printf("\" \"");
			else if (chars[i] < 128)
			{
			    if (chars[i] == 34)
				printf("\"&quot;\"");
			    else if (chars[i] == 38)
				printf("\"&amp;\"");
			    else if (chars[i] == 39)
				printf("\"&apos;\"");
			    else if (chars[i] == 60)
				printf("\"&lt;\"");
			    else if (chars[i] == 62)
				printf("\"&gt;\"");
			    else if (chars[i] == 92)
				printf("\"\\\\\"");
			    else
				printf("\"%c\"", chars[i]);
			}
			else
			    printf("\"&#%d;\"", chars[i]);
			i++;
			if (i < 256)
			    printf(", ");
		    }
		    printf("\n");
		}
		printf("};\n\n");
		printf("long %schars_bin[256] = {\n", simpname);
		i = 0;
		for (row = 0; row < 32; row++)
		{
		    for (col = 0; col < 8; col++)
		    {
			if (chars[i] >= 32 && chars[i] < 128)
			{
			    if (chars[i] == 39 || chars[i] == 92)
				printf("%c\\%c%c", 39, chars[i], 39);
			    else
				printf("%c%c%c", 39, chars[i], 39);
			}
			else if (i == 9)
			    printf("%c %c", 39, 39);
			else if (i == 10)
			    printf("%c\\n%c", 39, 39);
			else if (chars[i] == 0)
			    printf("%c %c", 39, 39);
			else
			    printf("%d", chars[i]);
			i++;
			if (i < 256)
			    printf(", ");
		    }
		    printf("\n");
		}
		printf("};\n\n");
		printf("int %schars_illegals[256] = {\n", simpname);
		i = 0;
		for (row = 0; row < 32; row++)
		{
		    for (col = 0; col < 8; col++)
		    {
			if (chars[i] == 0 && i != 9 && i != 10 && i != 13)
			    printf("1");
			else
			    printf("0");
			i++;
			if (i < 256)
			    printf(", ");
		    }
		    printf("\n");
		}
		printf("};\n\n");

	       }
	       '
