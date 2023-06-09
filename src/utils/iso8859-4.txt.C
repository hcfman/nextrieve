unsigned char *iso88594chars_str[256] = {
" ", " ", " ", " ", " ", " ", " ", " ", 
" ", " ", " ", " ", " ", " ", " ", " ", 
" ", " ", " ", " ", " ", " ", " ", " ", 
" ", " ", " ", " ", " ", " ", " ", " ", 
" ", "!", "&quot;", "#", "$", "%", "&amp;", "&apos;", 
"(", ")", "*", "+", ",", "-", ".", "/", 
"0", "1", "2", "3", "4", "5", "6", "7", 
"8", "9", ":", ";", "&lt;", "=", "&gt;", "?", 
"@", "A", "B", "C", "D", "E", "F", "G", 
"H", "I", "J", "K", "L", "M", "N", "O", 
"P", "Q", "R", "S", "T", "U", "V", "W", 
"X", "Y", "Z", "[", "\\", "]", "^", "_", 
"`", "a", "b", "c", "d", "e", "f", "g", 
"h", "i", "j", "k", "l", "m", "n", "o", 
"p", "q", "r", "s", "t", "u", "v", "w", 
"x", "y", "z", "{", "|", "}", "~", " ", 
" ", " ", " ", " ", " ", " ", " ", " ", 
" ", " ", " ", " ", " ", " ", " ", " ", 
" ", " ", " ", " ", " ", " ", " ", " ", 
" ", " ", " ", " ", " ", " ", " ", " ", 
"&#160;", "&#260;", "&#312;", "&#342;", "&#164;", "&#296;", "&#315;", "&#167;", 
"&#168;", "&#352;", "&#274;", "&#290;", "&#358;", "&#173;", "&#381;", "&#175;", 
"&#176;", "&#261;", "&#731;", "&#343;", "&#180;", "&#297;", "&#316;", "&#711;", 
"&#184;", "&#353;", "&#275;", "&#291;", "&#359;", "&#330;", "&#382;", "&#331;", 
"&#256;", "&#193;", "&#194;", "&#195;", "&#196;", "&#197;", "&#198;", "&#302;", 
"&#268;", "&#201;", "&#280;", "&#203;", "&#278;", "&#205;", "&#206;", "&#298;", 
"&#272;", "&#325;", "&#332;", "&#310;", "&#212;", "&#213;", "&#214;", "&#215;", 
"&#216;", "&#370;", "&#218;", "&#219;", "&#220;", "&#360;", "&#362;", "&#223;", 
"&#257;", "&#225;", "&#226;", "&#227;", "&#228;", "&#229;", "&#230;", "&#303;", 
"&#269;", "&#233;", "&#281;", "&#235;", "&#279;", "&#237;", "&#238;", "&#299;", 
"&#273;", "&#326;", "&#333;", "&#311;", "&#244;", "&#245;", "&#246;", "&#247;", 
"&#248;", "&#371;", "&#250;", "&#251;", "&#252;", "&#361;", "&#363;", "&#729;"
};

long iso88594chars_bin[256] = {
' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
' ', ' ', '\n', ' ', ' ', ' ', ' ', ' ', 
' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
' ', '!', '"', '#', '$', '%', '&', '\'', 
'(', ')', '*', '+', ',', '-', '.', '/', 
'0', '1', '2', '3', '4', '5', '6', '7', 
'8', '9', ':', ';', '<', '=', '>', '?', 
'@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 
'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 
'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 
'X', 'Y', 'Z', '[', '\\', ']', '^', '_', 
'`', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 
'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 
'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 
'x', 'y', 'z', '{', '|', '}', '~', ' ', 
' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
160, 260, 312, 342, 164, 296, 315, 167, 
168, 352, 274, 290, 358, 173, 381, 175, 
176, 261, 731, 343, 180, 297, 316, 711, 
184, 353, 275, 291, 359, 330, 382, 331, 
256, 193, 194, 195, 196, 197, 198, 302, 
268, 201, 280, 203, 278, 205, 206, 298, 
272, 325, 332, 310, 212, 213, 214, 215, 
216, 370, 218, 219, 220, 360, 362, 223, 
257, 225, 226, 227, 228, 229, 230, 303, 
269, 233, 281, 235, 279, 237, 238, 299, 
273, 326, 333, 311, 244, 245, 246, 247, 
248, 371, 250, 251, 252, 361, 363, 729
};

int iso88594chars_illegals[256] = {
1, 1, 1, 1, 1, 1, 1, 1, 
1, 0, 0, 1, 1, 0, 1, 1, 
1, 1, 1, 1, 1, 1, 1, 1, 
1, 1, 1, 1, 1, 1, 1, 1, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 1, 
1, 1, 1, 1, 1, 1, 1, 1, 
1, 1, 1, 1, 1, 1, 1, 1, 
1, 1, 1, 1, 1, 1, 1, 1, 
1, 1, 1, 1, 1, 1, 1, 1, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0, 
0, 0, 0, 0, 0, 0, 0, 0
};

