 {if ($1 == laststem)
    outline = outline " " $2;
  else
  {
    if (outline != "") print outline;
    outline = $1 " " $2;
    laststem = $1;
  }
}
END { if (outline != "") print outline;}
