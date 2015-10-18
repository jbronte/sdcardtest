// TODO:
//    - add signal handling, see if writes can be interrupted
//    - add rand+crc tests

/*!
 * @file sdtest.c
 * @brief Test application for SD Card longevity tests
 * @author
 * @date 2015-10-08
 *
 * Copyright (C) 2015 MicroPower Technologies Inc.
 * All Rights Reserved.
 * The information contained herein is confidential property of
 * MicroPower Technologies. The use, copying, transfer or disclosure
 * of such information is prohibited except by express written
 * agreement with MPT.
 */

//-----------------------------------------------------------------------------
// Includes
//-----------------------------------------------------------------------------
#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <linux/fs.h>
#include <limits.h>
#include <stdarg.h>
#include <malloc.h>

//-----------------------------------------------------------------------------
// Constant & Type Definitions
//-----------------------------------------------------------------------------
//#define LOG(format,args...) fprintf(G->logfd,"[%s]",G->devicename);if (G->timestamp) fprintf(G->logfd,"[%s]", gettime());fprintf(G->logfd," "format, ##args ); fflush(G->logfd);
#define LOG sdlog
#define DEFAULT_BUFFER_MODULO (1024*1024)
#define DEFAULT_BUFFER_SIZE   (DEFAULT_BUFFER_MODULO*128)
#define HERE printf("%s:%d\n",__FILE__,__LINE__);fflush(stdout);

typedef struct device_info_s
{
   uint64_t size;
   size_t   sectors;
   size_t   sector_size;
   size_t   sector_size_physical;
   size_t   sector_size_logical;
   size_t   min_io_size;
   size_t   opt_io_size;
   size_t   alignment_offset;
} device_info_t;

typedef enum
{
   ZERO =  1,
   RAND,
   MAX
} test_type_e;

typedef struct bwt_s
{
   struct timeval start_tv;
   uint64_t start_bytes;
   uint64_t result_bytes;
   uint64_t result_usecs;
} bwt_t;

typedef struct globals_s
{
   device_info_t     di;                  // struct to hold device information
   test_type_e       test_type;           // zero+ones or rand+crc
   char              *devicename;         // block device node /dev/sdX
   char              *statslogname;       // generated filename for log and stats
   int               dumpinfo;            // flag to dump device info
   int               verbose;             // flag to print each buffer IO
   int               timestamp;           // flag to add timestamps to outputs
   int               zerostats;           // flag to restart persistent stats counts
   int               logstdout;           // flag to log to stdout as well as file
   unsigned int      block_size;          // read/write blocks, same as buffer unless tiny partition
   unsigned int      block_writes;        // number of block_size in the device
   unsigned int      buffer_size;         // override for default block_size
   unsigned int      bandwidth_avg;
   uint64_t          pass_count;
   uint64_t          written_total;
   int               quitpasses;          // count to quit after N passes
   bwt_t             buffer_bw;           // timers and counts for buffer bandwidth
   FILE              *logfd;
} globals_t;

//-----------------------------------------------------------------------------
// Variable Declarations
//-----------------------------------------------------------------------------
static globals_t *G;

//-----------------------------------------------------------------------------
// Function Prototypes
//-----------------------------------------------------------------------------
static void usage(char *cmd);
static void parse_cmdline(globals_t *g, int argc, char **argv);
static int device_setup(globals_t *g);
static int device_test(globals_t *g);
static char *gettime();
static uint64_t measurebw(int start, uint64_t bytes, bwt_t *bwt);
static void stats_log_setup(globals_t *g);
static void mklogname(globals_t *g);
static void check_device_name(globals_t *g);
static void get_previous_counts(globals_t *g);
static uint32_t crc32(uint32_t crc, const void *buf, size_t size);
static void sdlog(const char* format, ... );

/*!
 * @brief Stats Log+Data Setup
 *
 */
static void stats_log_setup(globals_t *g)
{
   struct stat statbuf;
   mklogname(g);

   if (stat(g->statslogname, &statbuf) == 0)
   {
      if (!g->zerostats)
      {
         g->logfd = fopen(g->statslogname, "r+");
         check_device_name(g);
         get_previous_counts(g);
         return;
      }
   }
   // start new
   g->logfd = fopen(g->statslogname, "w+");
   if (!g->logfd)
   {
      fprintf(stderr, "ERROR: could not open %s\n", g->statslogname);
      exit(-1);
   }

   LOG("devicename=%s\n", g->devicename);
   LOG("starttime=%s\n", gettime());
   LOG("block_size=%u\n", g->block_size);
   LOG("block_writes=%u\n", g->block_writes);
   LOG("buffer_size=%u\n", g->buffer_size);
}

/*!
 * @brief Get Counts from Existing Log
 *
 */
static void get_previous_counts(globals_t *g)
{
   char str[200];
   char *token, *s1, *endptr;

   while(fgets(str, 200, g->logfd));
   if (!strstr(str, "stats"))
   {
      fprintf(stderr, "WARNING: log file doesn't have any data, starting from 0\n");
      return;
   }
   token= strtok_r(str, ":", &s1);
   token= strtok_r(s1, ":", &s1);
   g->written_total = strtoul(token,&endptr,0);
   token= strtok_r(s1, ":", &s1);
   g->pass_count = strtoul(token,&endptr,0);
   LOG("Restarting with Total written: %lu Pass count: %lu\n", g->written_total, g->pass_count);
}


/*!
 * @brief Main
 *
 */
int main(int argc, char **argv)
{
   if(geteuid()) {fprintf(stderr, "ERROR: must be root!\n");return -1;}
   G = (globals_t *)calloc(sizeof(globals_t),1);
   parse_cmdline(G, argc, argv);
   device_setup(G);
   stats_log_setup(G);

   if (G->test_type)
      device_test(G);

   return 0;
}

/*!
 * @brief Usage
 *
 * @param cmd     String command (argv[0])
 */
static void usage(char *cmd)
{
   printf("usage %s [options] device\n", cmd);
   printf("options:\n");
   printf("  -i               dump device info\n");
   printf("  -v               print each buffer I/O stats to output\n");
   printf("  -T               add timestamps to output\n");
   printf("  -Z               zero stats if present\n");
   printf("  -O               log to stdout as well as logfile\n");
   printf("  -t <test type>   where 'z' is zeroes/ones, 'r' is random with CRCs\n");
   printf("  -b <buffer size> override default buffer size of 134217728 (modulo 1048576) \n");
   printf("  -q <passes>      quit after number of passes\n");
   printf("  device           such as /dev/sdb or a partition /dev/sdb1\n");
}

/*!
 * @brief Parse command line
 *
 * @param g             pointer to globals
 * @param argc          argument count
 * @param argv          argument list
 */
static void parse_cmdline(globals_t *g, int argc, char **argv)
{
   int c;
   char *endptr;

   if (argc < 3) {
      usage(argv[0]); exit(-1);
   }

   // parse the command options
   while ((c = getopt(argc, argv, "hivTZOt:b:q:")) != -1)
      switch (c) {
         case 't': g->test_type = (optarg[0] == 'z') ? ZERO : \
                                  (optarg[0] == 'r') ? RAND : 0; break;
         case 'v': g->verbose++;                                 break;
         case 'i': g->dumpinfo++;                                break;
         case 'T': g->timestamp++;                               break;
         case 'Z': g->zerostats++;                               break;
         case 'O': g->logstdout++;                               break;
         case 'b': g->buffer_size = strtoul(optarg,&endptr,0);   break;
         case 'q': g->quitpasses = strtoul(optarg,&endptr,0);    break;
         case '?':
         case 'h':
         usage(argv[0]);
         exit(-1);
      }

   if (argc == optind)
   {
      fprintf(stderr, "ERROR: 'device' argument missing\n");
      usage(argv[0]);
      exit(-1);
   }
   g->devicename = strdup(argv[optind]);

   if (g->buffer_size %  DEFAULT_BUFFER_MODULO)
   {
      fprintf(stderr, "ERROR: 'buffer size' must be modulo 1048576\n");
      usage(argv[0]);
      exit(-1);
   }
}

/*!
 * @brief Device Setup
 *
 */
static int device_setup(globals_t *g)
{
   int fd;

   fd = open(g->devicename, O_RDWR);
   if (fd < 0)
   {
      LOG("could not open %s, exiting %d\n", g->devicename, fd);
      exit(-1);
   }

   ioctl(fd, BLKGETSIZE64, &g->di.size);
   ioctl(fd, BLKGETSIZE,   &g->di.sectors);
   ioctl(fd, BLKPBSZGET,   &g->di.sector_size_physical);
   ioctl(fd, BLKSSZGET,    &g->di.sector_size_logical);
   ioctl(fd, BLKIOMIN,     &g->di.min_io_size);
   ioctl(fd, BLKIOOPT,     &g->di.opt_io_size);
   ioctl(fd, BLKALIGNOFF,  &g->di.alignment_offset);

   if (!g->di.opt_io_size)
      g->di.opt_io_size = g->di.min_io_size;

   if (g->dumpinfo)
   {
      LOG("Dumping info for %s...\n", g->devicename);
      LOG("   size:                %lu\t(0x%lx)\n", g->di.size,g->di.size);
      LOG("   sectors:             %lu\t(0x%lx)\n", g->di.sectors,g->di.sectors);
      LOG("   physical block size: %lu\n", g->di.sector_size_physical);
      LOG("   logical block size:  %lu\n", g->di.sector_size_logical);
      LOG("   IO min size:         %lu\n", g->di.min_io_size);
      LOG("   IO opt size:         %lu\n", g->di.opt_io_size);
      LOG("   alignment offset:    %lu\n", g->di.alignment_offset);
   }

   if (!g->buffer_size)
      g->buffer_size = DEFAULT_BUFFER_SIZE;

   // find a reasonable size for read/write depending on device size
   if (g->di.size < g->buffer_size)
   {
      g->buffer_size = g->di.size;
      g->block_size = g->di.size;
      g->block_writes = 1;
   }
   else
   {
      g->block_size = g->buffer_size;
      g->block_writes = (unsigned int)(g->di.size / g->buffer_size);
   }

   close(fd);
   return 0;
}

/*!
 * @brief Device Test
 *
 */
static int device_test(globals_t *g)
{
   int fd;
   int rc = 0;
   unsigned char *rbuf;
   unsigned char *wbuf;
   unsigned int index = 0;
   uint64_t pass_wrbps =  0;
   uint64_t pass_rdbps = 0;
   uint64_t buffer_wrbps1;
   uint64_t buffer_wrbps2;
   uint64_t buffer_rdbps1;
   uint64_t buffer_rdbps2;

   fd = open(g->devicename, O_RDWR | __O_DIRECT);
   if (fd < 0)
   {
      LOG("could not open %s, exiting %d\n", g->devicename, fd);
      exit(-1);
   }

   rbuf = memalign(g->di.sector_size_logical, g->block_size);
   wbuf = memalign(g->di.sector_size_logical, g->block_size);

   while(1)
   {
      // a 'pass' is defined as the whole device (or partition)
      lseek(fd, 0, SEEK_SET);
      index = 0;

      LOG("stats:%lu:%lu:wrbw=%u.%02u MB/s:rdbw=%u.%02u MB/s\n",
         g->written_total,
         g->pass_count,
         (unsigned int)pass_wrbps/1000000,
         (unsigned int)pass_wrbps%1000000,
         (unsigned int)pass_rdbps/1000000,
         (unsigned int)pass_rdbps%1000000);

      if (g->quitpasses && (g->pass_count >= g->quitpasses))
         goto done;

      // within each pass are blocks, where each block is tested
      while(index < g->block_writes)
      {
         // write ones:
         lseek(fd, index*g->block_size, SEEK_SET);
         memset(wbuf, 0xFF, g->block_size);
         measurebw(1, 0, &g->buffer_bw);
         write(fd, wbuf, g->block_size);
         buffer_wrbps1 = measurebw(0, g->block_size, &g->buffer_bw);
         g->written_total += g->block_size;

         if(g->verbose) {
            LOG("stats:%lu:%lu:wrbw=%u.%02u MB/s:rdbw=%u.%02u MB/s",
               g->written_total,
               g->pass_count,
               (unsigned int)pass_wrbps/1000000,
               (unsigned int)pass_wrbps%1000000,
               (unsigned int)pass_rdbps/1000000,
               (unsigned int)pass_rdbps%1000000);
            LOG(":buffer stats:W1:%lu:%lu:%u.%02u MB/s\n",
               g->buffer_bw.result_bytes,
               g->buffer_bw.result_usecs,
               (unsigned int)(buffer_wrbps1/1000000),
               (unsigned int)(buffer_wrbps1%1000000));
         }

         // read ones and check:
         lseek(fd, index*g->block_size, SEEK_SET);
         measurebw(1, 0, &g->buffer_bw);
         read(fd, rbuf, g->block_size);
         buffer_rdbps1 = measurebw(0, g->block_size, &g->buffer_bw);

         if (memcmp(rbuf,wbuf,g->block_size))
         {
            LOG("error at block %d, exiting...\n", index);
            rc = -1;
            goto done;
         }
         if(g->verbose) {
            LOG("stats:%lu:%lu:wrbw=%u.%02u MB/s:rdbw=%u.%02u MB/s",
               g->written_total,
               g->pass_count,
               (unsigned int)pass_wrbps/1000000,
               (unsigned int)pass_wrbps%1000000,
               (unsigned int)pass_rdbps/1000000,
               (unsigned int)pass_rdbps%1000000);
            LOG(":buffer stats:R1:%lu:%lu:%u.%02u MB/s\n",
               g->buffer_bw.result_bytes,
               g->buffer_bw.result_usecs,
               (unsigned int)(buffer_rdbps1/1000000),
               (unsigned int)(buffer_rdbps1%1000000));
         }

         // write zeroes:
         lseek(fd, index*g->block_size, SEEK_SET);
         memset(wbuf, 0, g->block_size);
         measurebw(1, 0, &g->buffer_bw);
         write(fd, wbuf, g->block_size);
         buffer_wrbps2 = measurebw(0, g->block_size, &g->buffer_bw);
         g->written_total += g->block_size;

         if(g->verbose) {
            LOG("stats:%lu:%lu:wrbw=%u.%02u MB/s:rdbw=%u.%02u MB/s",
               g->written_total,
               g->pass_count,
               (unsigned int)pass_wrbps/1000000,
               (unsigned int)pass_wrbps%1000000,
               (unsigned int)pass_rdbps/1000000,
               (unsigned int)pass_rdbps%1000000);
            LOG(":buffer stats:W2:%lu:%lu:%u.%02u MB/s\n",
               g->buffer_bw.result_bytes,
               g->buffer_bw.result_usecs,
               (unsigned int)(buffer_wrbps2/1000000),
               (unsigned int)(buffer_wrbps2%1000000));
         }

         // read zeroes and check:
         lseek(fd, index*g->block_size, SEEK_SET);
         measurebw(1, 0, &g->buffer_bw);
         read(fd, rbuf, g->block_size);
         buffer_rdbps2 = measurebw(0, g->block_size, &g->buffer_bw);

         if (memcmp(rbuf,wbuf,g->block_size))
         {
            LOG("error at block %d, exiting...\n", index);
            rc = -1;
            goto done;
         }
         if(g->verbose) {
            LOG("stats:%lu:%lu:wrbw=%u.%02u MB/s:rdbw=%u.%02u MB/s",
               g->written_total,
               g->pass_count,
               (unsigned int)pass_wrbps/1000000,
               (unsigned int)pass_wrbps%1000000,
               (unsigned int)pass_rdbps/1000000,
               (unsigned int)pass_rdbps%1000000);
            LOG(":buffer stats:R2:%lu:%lu:%u.%02u MB/s\n",
               g->buffer_bw.result_bytes,
               g->buffer_bw.result_usecs,
               (unsigned int)(buffer_rdbps2/1000000),
               (unsigned int)(buffer_rdbps2%1000000));
         }
         index++;
      } /* end full pass */

      pass_wrbps = (buffer_wrbps1 + buffer_wrbps2) / 2;
      pass_rdbps = (buffer_rdbps1 + buffer_rdbps2) / 2;
      g->pass_count++;
   } /* end while(1) */

done:
   free(wbuf);
   free(rbuf);
   close(fd);
   return rc;
}

/*!
 * @brief Utility to measure bandwidth of operations
 *
 * @param start         Flag to start timer
 * @param bwtime        Struct for this timer instance
 */
static uint64_t measurebw(int start, uint64_t bytes, bwt_t *bwt)
{
   struct timezone tz;
   struct timeval tv2;
   uint64_t t1,t2;

   if (start)
   {
      bwt->start_bytes = bytes;
      gettimeofday(&bwt->start_tv, &tz);
   }
   else
   {
      gettimeofday(&tv2, &tz);
      t1 = bwt->start_tv.tv_sec * 1000000 + bwt->start_tv.tv_usec;
      t2 = tv2.tv_sec * 1000000 + tv2.tv_usec;
      bwt->result_usecs = (t2 - t1);
      bwt->result_bytes = bytes - bwt->start_bytes;
      // probably should be float, but what the heck...
      if ( !bwt->result_bytes || !bwt->result_usecs)
         return 0;
      else
         return ( bwt->result_bytes * 1000000 / bwt->result_usecs );
   }
   return 0;
}

/*!
 * @brief Log Utility
 *
 */
static void sdlog(const char* format, ... )
{
   char sdmsg[120];
   va_list args;
   va_start( args, format );

   sprintf(sdmsg, "[%s]", G->devicename);;
   if (G->timestamp)
      sprintf(&sdmsg[strlen(sdmsg)],"[%s]", gettime());
   strcat(sdmsg, " ");
   vsprintf(&sdmsg[strlen(sdmsg)], format, args );
   if (G->logstdout || !G->logfd)
      printf("%s",sdmsg);fflush(stdout);
   if (G->logfd)
      fprintf(G->logfd,"%s",sdmsg);fflush(G->logfd);
   va_end( args );
}

/*!
 * @brief Make Filename for Stats Log and Data
 *
 */
static void mklogname(globals_t *g)
{
   g->statslogname = calloc(128,1);

   // assume all devices will be "/dev/sdXX"
   if (strncmp(g->devicename, "/dev/sd", 7))
   {
      fprintf(stderr, "ERROR: device must be /dev/sd[XX]\n");
      exit(-1);
   }
   strcpy(g->statslogname,&g->devicename[5]);
   strcat(g->statslogname, ".log");
}

/*!
 * @brief Check Device Name - parses first line for "[/dev/sdX]"
 *
 */
static void check_device_name(globals_t *g)
{
   char devicestr[80];

   if (fgets(devicestr, 80, g->logfd) == 0)
   {
      fprintf(stderr, "ERROR: cannot read log file %s\n", g->statslogname);
      exit(-1);
   }

   if (memcmp(&devicestr[1], g->devicename, strlen(g->devicename)))
   {
      fprintf(stderr, "ERROR: log file device %s doesn't match active device %s\n", devicestr, g->devicename);
      exit(-1);
   }
}

/*!
 * @brief Get Time
 *
 */
static char *gettime()
{
   // do as I say, not as I do...
   time_t t;
   char delim[] = "\n";
   char *tstr;
   time(&t);
   tstr = ctime(&t);
   return strtok(tstr, delim);
}

static uint32_t crc32_tab[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

/*!
 * @brief Calculate CRC32
 *
 */
static uint32_t crc32(uint32_t crc, const void *buf, size_t size)
{
	const uint8_t *p;

	p = buf;
	crc = crc ^ ~0U;

	while (size--)
		crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);

	return crc ^ ~0U;
}


/*================================== EOF ====================================*/
