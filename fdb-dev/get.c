#include <stdio.h>

#include "filedb.h"
#include "files.c"

#include "tsc.h"

int main()
{
  gint fd,j,id;
  
  start_timer(0);
  if (fdb_open("."))
  {
    printf("%s\n", fdb_error());
    exit(1);
  }
  stop_timer(0);
  print_timer(0, "open");

  start_timer(0);
  for (j=0; j<sizeof(files)/sizeof(files[0]); j++)
  {
    id = fdb_get_file_id(files[j]);

    if (id == 0)
    {
      printf("error\n");
      break;
    }
  }
  stop_timer(0);
  print_timer(0, "get");

  fdb_close();

  return 0;
}
