/*****************************************************************************
  Copyright (C) 2018 Fillet
 
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
 
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111, USA.
 
  This program is also available under a commercial license with
  customization/support packages and additional features.  For more 
  information, please contact us at cannonbeachgoonie@gmail.com

******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <math.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <execinfo.h>
#include "fillet.h"
#include "fgetopt.h"
#include "dataqueue.h"
#include "mempool.h"
#include "udpsource.h"
#include "tsreceive.h"
#include "tsdecode.h"
#include "hlsmux.h"
#include "mp4core.h"
#include "background.h"

#define SOURCE_NONE      -1
#define SOURCE_IP        1
#define SOURCE_FILE      2

static int calculated_mux_rate = 0;
static error_struct error_data[MAX_ERROR_SIZE];
static int error_count = 0;
static int video_synchronizer_entries = 0;
static int audio_synchronizer_entries = 0;
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static int quit_sync_thread = 0;
static int sync_thread_running = 0;
static pthread_t frame_sync_thread_id;
static int enable_background = 0;
static int enable_verbose = 0;

static volatile int child_pids[MAX_SESSIONS];
static config_options_struct config_data;

static struct option long_options[] = {
     {"sources", required_argument, 0, 'S'},
     {"ip", required_argument, 0, 'i'},
     {"verbose", no_argument, &enable_verbose, 1},
     {"interface", required_argument, 0, 'f'},
     {"window", required_argument, 0, 'w'},
     {"segment", required_argument, 0, 's'},
     {"manifest", required_argument, 0, 'm'},
     {"rollover", required_argument, 0, 'r'},
     {"identity", required_argument, 0, 'u'},
     {"background", no_argument, &enable_background, 1},
     {0, 0, 0, 0}
};

void crash_handler(int sig)
{
#define MAX_TRACE 10    
    void *array[MAX_TRACE];
    size_t size;

    size = backtrace(array, MAX_TRACE);

    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);

    exit(1);
}

static int destroy_fillet_core(fillet_app_struct *core)
{
    int num_sources;
    int current_source;
    
    if (!core) {
	return -1;
    }

    num_sources = core->num_sources;
    for (current_source = 0; current_source < num_sources; current_source++) {
	video_stream_struct *vstream;
	audio_stream_struct *astream;
	int audio_source;

	for (audio_source = 0; audio_source < MAX_AUDIO_SOURCES; audio_source++) {
	    astream = (audio_stream_struct*)core->source_stream[current_source].audio_stream[audio_source];
	    dataqueue_destroy(astream->audio_queue);
	    
	    free(core->source_stream[current_source].audio_stream[audio_source]);
	    core->source_stream[current_source].audio_stream[audio_source] = NULL;
	}
	vstream = (video_stream_struct*)core->source_stream[current_source].video_stream;
	dataqueue_destroy(vstream->video_queue);
	free(core->source_stream[current_source].video_stream);
	core->source_stream[current_source].video_stream = NULL;		 
    }
    dataqueue_destroy(core->event_queue);
    core->event_queue = NULL;
    sem_destroy(core->event_wait);
    core->event_wait = NULL;
    free(core->event_wait);
    free(core);

    return 0;
}

static fillet_app_struct *create_fillet_core(config_options_struct *cd, int num_sources)
{
    fillet_app_struct *core;
    int current_source;

    core = (fillet_app_struct*)malloc(sizeof(fillet_app_struct));
    if (!core) {
	return NULL;
    }

    core->num_sources = num_sources;
    core->cd = cd;
    core->source_stream = (source_stream_struct*)malloc(sizeof(source_stream_struct)*num_sources);
    core->event_queue = (void*)dataqueue_create();
    core->event_wait = (sem_t*)malloc(sizeof(sem_t));
    sem_init(core->event_wait, 0, 0);
    
    memset(core->source_stream, 0, sizeof(source_stream_struct)*num_sources);
    
    for (current_source = 0; current_source < num_sources; current_source++) {
	video_stream_struct *vstream;
	audio_stream_struct *astream;
	int audio_source;

	for (audio_source = 0; audio_source < MAX_AUDIO_SOURCES; audio_source++) {
	    core->source_stream[current_source].audio_stream[audio_source] = (audio_stream_struct*)malloc(sizeof(audio_stream_struct));
	    if (!core->source_stream[current_source].audio_stream[audio_source]) {
		goto _fillet_create_fail;
	    }	    
	    astream = (audio_stream_struct*)core->source_stream[current_source].audio_stream[audio_source];
	    memset(astream, 0, sizeof(audio_stream_struct));
	    astream->audio_queue = (void*)dataqueue_create();
	    astream->last_timestamp_pts = -1;
	}
	
	core->source_stream[current_source].video_stream = (video_stream_struct*)malloc(sizeof(video_stream_struct));
	if (!core->source_stream[current_source].video_stream) {
	    goto _fillet_create_fail;
	}
	
	vstream = (video_stream_struct*)core->source_stream[current_source].video_stream;
	memset(vstream, 0, sizeof(video_stream_struct));
	vstream->last_timestamp_pts = -1;
	vstream->last_timestamp_dts = -1;
	vstream->video_queue = (void*)dataqueue_create();
    }

    return core;

_fillet_create_fail:

    destroy_fillet_core(core);
    
    return NULL;
}

static int parse_input_options(int argc, char **argv)
{
     if (argc == 1) {
	 return -1;
     }

     while (1) {
          int option_index = -1;
          int c;

          c = fgetopt_long(argc,
                           argv,
                           "w:s:f:i:S:r:u",
                           long_options,
                           &option_index);

          if (c == -1) {
               break;
          }

          switch (c) {
	  case 'u':
	      if (optarg) {
		  config_data.identity = atoi(optarg);
		  if (config_data.identity < 0) {
		      fprintf(stderr,"ERROR: INVALID CONFIG IDENTITY: %d\n", config_data.identity);
		      return -1;
		  }
		  fprintf(stderr,"STATUS: Using config identity: %d\n", config_data.identity);
	      }
	      break;
	  case 'w':
	      if (optarg) {
		  config_data.window_size = atoi(optarg);
		  if (config_data.window_size < MIN_WINDOW_SIZE || config_data.window_size > MAX_WINDOW_SIZE) {
		      fprintf(stderr,"ERROR: INVALID WINDOW SIZE: %d\n", config_data.window_size);
		      return -1;
		  }
		  fprintf(stderr,"STATUS: Using window size: %d\n", config_data.window_size);
	      }
	      break;
	  case 'r':
	      if (optarg) {
		  config_data.rollover_size = atoi(optarg);
		  if (config_data.rollover_size < MIN_ROLLOVER_SIZE || config_data.rollover_size > MAX_ROLLOVER_SIZE) {
		      fprintf(stderr,"ERROR: INVALID ROLLOVER SIZE: %d\n", config_data.rollover_size);
		      return -1;
		  }
		  fprintf(stderr,"STATUS: Using rollover size: %d\n", config_data.rollover_size);
	      }
	      break;
	  case 's':
	      if (optarg) {
		  config_data.segment_length = atoi(optarg);
		  if (config_data.segment_length < MIN_SEGMENT_LENGTH || config_data.segment_length > MAX_SEGMENT_LENGTH) {
		      fprintf(stderr,"ERROR: INVALID SEGMENT LENGTH: %d\n", config_data.segment_length);
		      return -1;
		  }
		  fprintf(stderr,"STATUS: Using segment length: %d\n", config_data.segment_length);
	      }
	      break;
	  case 'f':
	      if (optarg) {
		  snprintf((char*)config_data.active_interface,UDP_MAX_IFNAME-1,"%s",optarg);
		  fprintf(stderr,"STATUS: Using supplied interface: %s\n", config_data.active_interface);
	      } else {
		  fprintf(stderr,"STATUS: No interface provided - defaulting to loopback (lo)\n");
		  snprintf((char*)config_data.active_interface,UDP_MAX_IFNAME-1,"lo");
	      }
	      break;
	  case 'm':
	      if (optarg) {
		  int slen;
		  
		  snprintf((char*)config_data.manifest_directory,MAX_STR_SIZE-1,"%s",optarg);
		  slen = strlen(config_data.manifest_directory)-1;
		  if (config_data.manifest_directory[slen] == '/') {
		      config_data.manifest_directory[slen] = '\0';
		      fprintf(stderr,"STATUS: Removing trailing '/' from directory name\n");
		  }
		  
		  fprintf(stderr,"STATUS: Using supplied manifest directory: %s\n", config_data.manifest_directory);
	      } else {
		  fprintf(stderr,"STATUS: No manifest directory supplied - using default: %s\n", config_data.manifest_directory);
	      }
	      break;
	  case 'i': // ip addresses
	      if (optarg) {
		  int l;
		  int f;
		  int a;
		  int p;
		  char current_ip[UDP_MAX_IFNAME];
		  char current_port[UDP_MAX_IFNAME];
		  int optlen = strlen(optarg);
		  int parsing_port = 0;
		  
		  memset(current_ip,0,sizeof(current_ip));
		  memset(current_port,0,sizeof(current_port));
		  a = 0;
		  f = 0;
		  p = 0;
		  for (l = 0; l < optlen; l++) {
		      int colon = 0;
		      
		      if (optarg[l] != ',' && optarg[l] != '\0') {
			  if (optarg[l] == ':') {
			      parsing_port = 1;
			      colon = 1;
			  }

			  if (parsing_port) {
			      if (!colon) {
				  current_port[p] = optarg[l];
				  p++;
			      }
			  } else {
			      current_ip[f] = optarg[l];
			      f++;
			  }
		      } else {			  
			  parsing_port = 0;
			  snprintf(config_data.active_source[a].active_ip,UDP_MAX_IFNAME-1,"%s",current_ip);
			  config_data.active_source[a].active_port = atol(current_port);
			  fprintf(stderr,"STATUS: Source %d IP: %s:%d\n",
				  a,
				  config_data.active_source[a].active_ip,
				  config_data.active_source[a].active_port);
			  f = 0;
			  p = 0;
			  a++;
		      }
		  }

		  snprintf(config_data.active_source[a].active_ip,UDP_MAX_IFNAME-1,"%s",current_ip);
		  config_data.active_source[a].active_port = atol(current_port);
		  fprintf(stderr,"STATUS: Source %d IP: %s:%d\n",
			  a,
			  config_data.active_source[a].active_ip,
			  config_data.active_source[a].active_port);
		  a++;		  

		  for (f = 0; f < a; f++) {
		      struct in_addr addr;
		      
		      if (config_data.active_source[f].active_port == 0 ||
			  config_data.active_source[f].active_port >= 65535) {
			  fprintf(stderr,"ERROR: INVALID PORT SELECTED: %d\n",
				  config_data.active_source[f].active_port);
			  return -1;
		      }

		      if (inet_aton(config_data.active_source[f].active_ip, &addr) == 0) {
			  fprintf(stderr,"ERROR: INVALID IP ADDRESS: %s\n",
				  config_data.active_source[f].active_ip);
			  return -1;
		      }		      
		  }
	      }
	      break;
	  case 'S':
	      if (optarg) {
		  config_data.active_sources = atoi(optarg);
		  if (config_data.active_sources < 0 ||
		      config_data.active_sources > 10) {
		      fprintf(stderr,"ERROR: INVALID NUMBER OF SOURCES: %d\n", config_data.active_sources);
		      return -1;
		  }
	      } else {
		  return -1;
	      }
	      break;
          }
     }
     return 0;
}

const float framerate_lut[9] = {0.000f,
                                23.976f,
                                24.000f,
                                25.000f,
                                29.970f,
                                30.000f,
                                50.000f,
                                59.940f,
                                60.000f
                               };


static int message_dispatch(int p1, int64_t p2, int64_t p3, int64_t p4, int64_t p5, int source, void *context)
{
    //fillet_app_struct *core = (fillet_app_struct*)context;
     int message_handled = 1;

     if (p1 == 2000) {
          switch (p2) {
          case 100:
               fprintf(stderr,"PAT TABLE FOUND\n");
               break;
          case 200:
 	       fprintf(stderr,"PMT TABLE FOUND ON PID:%d\n", (int)p3);
               break;
          case 400:  // GET THE CALCULATED MUX RATE
               calculated_mux_rate = p4;
               break;
          case 600:
               //fprintf(stderr,"UNKNOWN DESCRIPTOR RECEIVED: 0x%x FOR PID:%d\n", p3, p4);
               break;
          case 610:
               //fprintf(stderr,"PMT REGISTRATION DESCRIPTOR 0x%x FOR PID:%d\n", p3, p4);
               break;
          case 709:
               //fprintf(stderr,"VIDEO ALIGNMENT DESCRIPTOR FOUND (0x06) - ALIGNMENT-TYPE:%d\n", p3);
               break;
          case 711:
               //fprintf(stderr,"H.264 VIDEO DESCRIPTOR (0x28) FOR PID:%d\n", p4);
               break;
          case 715:
               //fprintf(stderr,"SMOOTHING BUFFER DESCRIPTOR - TAG:0x%x\n", p3);
               break;
          case 717:
               //fprintf(stderr,"REGISTRATION DESCRIPTOR - TAG:0x%x\n", p3);
               break;
          case 718:
               //fprintf(stderr,"AC3 AUDIO DESCRIPTOR - TAG:0x%x\n", p3);
               break;
          case 719:
	       {
		   fprintf(stderr,"PMT DECODED: LANGUAGE DESCRIPTOR: %c%c%c\n", (char)p4, (char)p5, (char)source);
	       }
               break;
          case 799:
               //fprintf(stderr,"STREAM DESCRIPTOR - TAG:0x%x\n", p3);
               break;
          case 800:
               //fprintf(stderr,"MPEG2 VIDEO STREAM FOUND ON PID:%d BELONGS TO PMT:%d\n", p3, p4);
               break;
          case 801:
               //fprintf(stderr,"H.264 VIDEO STREAM FOUND ON PID:%d BELONGS TO PMT:%d\n", p3, p4);
               break;
          case 805:
               //fprintf(stderr,"AAC AUDIO STREAM FOUND ON PID:%d BELONGS TO PMT:%d\n", p3, p4);
               break;
          case 806:
               //fprintf(stderr,"AC3 AUDIO STREAM FOUND ON PID:%d BELONGS TO PMT:%d\n", p3, p4);
               break;
          case 812:
               //fprintf(stderr,"SCTE35 SPLICE PID:%d BELONGS TO PMT:%d\n", p3, p4);
               break;
          case 900:
          case 901:
          case 902:
          case 903:
          case 904:
          case 905:
          case 906:
          case 907:
          case 908:
          case 909:
          case 910:
          case 911:
          case 912:
          case 913:
          case 914:
          case 915:
               //fprintf(stderr,"CONTINUITY COUNTER ERROR ON PID:%d  EXPECTED:%d INSTEAD FOUND:%d\n", p4, p3, p2 - 900);
               error_data[error_count].packet_number = p5;
               error_data[error_count].pid = p4;
               sprintf(error_data[error_count].error_message,"CONTINUITY COUNTER ERROR ON PID:%4d (0x%4x)  LOOKING FOR %2d BUT FOUND %2d",
                       (int)p4, (int)p4,
                       (int)p3, (int)p2 - 900);
               error_count = (error_count + 1) % MAX_ERROR_SIZE;
               break;
          case 1000: {
               int framecode = p3;
               if (framecode > 8 || framecode < 0) {
                    framecode = 0;
               }

               fprintf(stderr,"PUBLISHED VIDEO FRAMERATE FOR PID(%d) IS: %.2f fps\n",
                       (int)p4, framerate_lut[framecode]);
          }
          break;
          case 1001: {
               fprintf(stderr,"PUBLISHED BITRATE FOR PID(%d) IS: %d bps\n",
                       (int)p4, (int)p3);
          }
          break;
          case 1002: {
	       fprintf(stderr,"PUBLISHED RESOLUTION FOR PID(%d) IS: %d x %d\n",
                       (int)p5, (int)p3, (int)p4);
          }
          break;
          case 1003: {
               if (p4 == 1) {
                    fprintf(stderr,"PUBLISHED SOURCE STREAM IS PROGRESSIVE FOR PID(%ld)\n", p5);
               } else {
                    fprintf(stderr,"PUBLISHED SOURCE STREAM IS INTERLACED FOR PID(%ld)\n", p5);
               }
               if (p3 == 0) {
                    fprintf(stderr,"PUBLISHED ASPECT RATIO IS UNDEFINED\n");
               } else if (p3 == 1) {
                    fprintf(stderr,"PUBLISHED ASPECT RATIO IS 1:1\n");
               } else if (p3 == 2) {
                    fprintf(stderr,"PUBLISHED ASPECT RATIO IS 4:3\n");
               } else if (p3 == 3) {
                    fprintf(stderr,"PUBLISHED ASPECT RATIO IS 16:9\n");
               } else {
                    fprintf(stderr,"PUBLISHED ASPECT RATIO IS UNKNOWN\n");
               }
          }
          break;
          default:
               message_handled = 0;
               break;
          }
     } else {
          message_handled = 0;
     }

     if (!(p1 >= 4000 && p1 <= 4500)) {
          if (!message_handled) {
               fprintf(stderr,"STATUS(%d)  P2=%ld  P3=%ld  P4=%ld  P5=%ld  CONTEXT:%p\n",
                       p1, p2, p3, p4, p5, context);
          }
     }
     return 0;
}

int peek_frame(sorted_frame_struct **frame_data, int entries, int pos, int64_t *current_time, int *sync)
{
    sorted_frame_struct *get_frame;

    pos = 0;
    get_frame = (sorted_frame_struct*)frame_data[pos];
    if (get_frame) {
	if (get_frame->frame_type == FRAME_TYPE_VIDEO) {
	    *current_time = get_frame->full_time;
	    *sync = get_frame->sync_frame;
	} else {
	    *current_time = get_frame->full_time;
	    *sync = 1;
	}
	return 0;
    }
    return -1;    
}

// scrub_frames - anything older than 15 seconds gets scrubbed

int use_frame(sorted_frame_struct **frame_data, int entries, int pos, int64_t *current_time, int dump_sample, sorted_frame_struct **output_frame)
{
    int i;
    sorted_frame_struct *get_frame;

    // start from the top?
    pos = 0;
    get_frame = (sorted_frame_struct*)frame_data[pos];
    for (i = pos; i < entries; i++) {	
        frame_data[i] = frame_data[i+1];
    }
    
    if (get_frame->frame_type == FRAME_TYPE_VIDEO) {
	*current_time = get_frame->full_time;	
    } else {
	*current_time = get_frame->full_time;
    }	    

    if (!dump_sample) {
	*output_frame = get_frame;
    } else {
	*output_frame = NULL;
	free(get_frame);
    }
 
    return entries - 1;
}

int add_frame(sorted_frame_struct **frame_data, int entries, sorted_frame_struct *new_frame, int capacity)
{
    int i;
    int new_count;

    if (entries >= capacity) {
	return entries;
    }

    for (i = entries - 1; (i >= 0 && frame_data[i]->full_time > new_frame->full_time); i--) {
	frame_data[i+1] = frame_data[i];
    } 
    frame_data[i+1] = (sorted_frame_struct*)new_frame;
    
    new_count = entries + 1;
    
    return new_count;
}

int dump_frames(sorted_frame_struct **frame_data, int max)
{
    int i;

    fprintf(stderr,"-------------- dumping out frame data ------------\n");
    for (i = 0; i < max; i++) {
	if (frame_data[i]) {
	    if (frame_data[i]->frame_type == FRAME_TYPE_VIDEO) {
		fprintf(stderr,"[I:%4d] VIDEO:  SOURCE:%d PTS:%ld\n",
			i,
			frame_data[i]->source,
			frame_data[i]->full_time);
		free(frame_data[i]->buffer);
		frame_data[i]->buffer = NULL;
		free(frame_data[i]);
		frame_data[i] = NULL;	    	    
	    } else {	    
		fprintf(stderr,"[I:%4d] AUDIO:  SOURCE:%d PTS:%ld\n",
			i,
			frame_data[i]->source,
			frame_data[i]->full_time);
		free(frame_data[i]->buffer);
		frame_data[i]->buffer = NULL;
		free(frame_data[i]);
		frame_data[i] = NULL;
	    }
	} else {
	    //fprintf(stderr,"[I:%4d] NO FRAME DATA\n", i);
	}
    }
    if (enable_background) {
        exit(0);
    }
    return 0;
}

static void *frame_sync_thread(void *context)
{
    fillet_app_struct *core = (fillet_app_struct*)context;
    sorted_frame_struct *output_frame = NULL;
    int64_t current_video_time = 0;
    int64_t current_audio_time = 0;
    int audio_sync = 0;
    int video_sync = 0;
    int first_grab = 1;
    int no_grab = 0;
    int source_discontinuity = 1;
    int print_entries = 0;
    int print_current_time = 0;

    fprintf(stderr,"SESSION:%d (MAIN) STATUS: STARTING NEW SYNC THREAD\n", core->session_id);
    while (1) {
	audio_sync = 0;
	video_sync = 0;

	if (quit_sync_thread) {
	    pthread_mutex_lock(&sync_lock);		    	    
	    dump_frames(core->video_frame_data, MAX_FRAME_DATA_SYNC_VIDEO);
	    dump_frames(core->audio_frame_data, MAX_FRAME_DATA_SYNC_AUDIO);
	    pthread_mutex_unlock(&sync_lock);
	    video_synchronizer_entries = 0;
	    audio_synchronizer_entries = 0;	    
	    fprintf(stderr,"SESSION:%d (MAIN) STATUS: LEAVING SYNC THREAD - DISCONTINUITY\n", core->session_id);
            sync_thread_running = 0;            
	    return NULL;
	}

	// if audio > video, then not yet ready...
        if (enable_verbose) {
            if (print_entries > 200) {
                fprintf(stderr,"SESSION:%d (MAIN) STATUS: audio_synchronizer_entries:%d  video_synchronizer_entries:%d\n",
                        core->session_id,
                        audio_synchronizer_entries,
                        video_synchronizer_entries);
                print_entries = 0;
            }
            print_entries++;
        }
            
	if (audio_synchronizer_entries > config_data.active_sources && video_synchronizer_entries > config_data.active_sources) {
	    output_frame = NULL;
	    
	    pthread_mutex_lock(&sync_lock);	
	    peek_frame(core->audio_frame_data, audio_synchronizer_entries, 0, &current_audio_time, &audio_sync);
	    peek_frame(core->video_frame_data, video_synchronizer_entries, 0, &current_video_time, &video_sync);	
	    pthread_mutex_unlock(&sync_lock);

            if (enable_verbose) {
                if (print_current_time > 200) {
                    fprintf(stderr,"SESSION:%d (MAIN) STATUS: current_audio_time:%ld current_video_time:%ld\n",
                            core->session_id,
                            current_audio_time,
                            current_video_time);
                    print_current_time = 0;
                }
                print_current_time++;
            }
            
	    if (current_audio_time <= current_video_time) {
		no_grab = 0;
		while (current_audio_time < current_video_time && audio_synchronizer_entries > config_data.active_sources && !quit_sync_thread) {
		    pthread_mutex_lock(&sync_lock);
		    audio_synchronizer_entries = use_frame(core->audio_frame_data, audio_synchronizer_entries, 0, &current_audio_time, first_grab, &output_frame);
		    pthread_mutex_unlock(&sync_lock);
		    if (output_frame) {
			dataqueue_message_struct *msg;			
			msg = (dataqueue_message_struct*)malloc(sizeof(dataqueue_message_struct));
			if (msg) {
			    msg->buffer = output_frame;
			    msg->source_discontinuity = source_discontinuity;
			    source_discontinuity = 0;
			}
			dataqueue_put_front(core->hlsmux->input_queue, msg);
		    }
		}
	    } else {
		fprintf(stderr,"NOT GRABBING AUDIO: DELTA:%ld (CVT:%ld CAT:%ld) AS:%d ASE:%d VSE:%d\n",
			current_video_time - current_audio_time,
			current_video_time,
			current_audio_time,
			config_data.active_sources,
			audio_synchronizer_entries,
			video_synchronizer_entries);
		first_grab = 0;

		no_grab++;
		if (no_grab >= 15) {
		    quit_sync_thread = 1;
		    continue;
		}		
	    }

            if (quit_sync_thread) {
                continue;
            }
			
	    if (!first_grab) {
		pthread_mutex_lock(&sync_lock);
		video_synchronizer_entries = use_frame(core->video_frame_data, video_synchronizer_entries, 0, &current_video_time, first_grab, &output_frame);
		pthread_mutex_unlock(&sync_lock);

		if (output_frame) {
		    dataqueue_message_struct *msg;
		    msg = (dataqueue_message_struct*)malloc(sizeof(dataqueue_message_struct));
		    if (msg) {
			msg->buffer = output_frame;
			msg->source_discontinuity = source_discontinuity;
			source_discontinuity = 0;
		    }
		    dataqueue_put_front(core->hlsmux->input_queue, msg);
		}
	    }
	} else {
	    usleep(5000);
	}
    }

    //never gets here
    return NULL;
}

int64_t time_difference(struct timespec *now, struct timespec *start)
{
    int64_t tsec;
    int64_t tnsec;
    
    if (now->tv_nsec < start->tv_nsec) {
	tsec = (now->tv_sec - start->tv_sec);
	tsec--;
	tnsec = 1000000000;
	tnsec += (now->tv_nsec - start->tv_nsec);
    } else {
	tsec = now->tv_sec - start->tv_sec;
	tnsec = now->tv_nsec - start->tv_nsec;
    }
    
    return ((tnsec / 1000) + (tsec * 1000000));
}

static int receive_frame(uint8_t *sample, int sample_size, int sample_type, uint32_t sample_flags, int64_t pts, int64_t dts, int64_t last_pcr, int source, int sub_source, char *lang_tag, void *context)
{
    fillet_app_struct *core = (fillet_app_struct*)context;
    int restart_sync_thread = 0;

    if (sample_type == STREAM_TYPE_MPEG2) {
	video_stream_struct *vstream = (video_stream_struct*)core->source_stream[source].video_stream;
	
	if (!vstream->found_key_frame && sample_flags) {
	    vstream->found_key_frame = 1;
	    if (dts > 0) {
		vstream->first_timestamp = dts;
	    } else {
		vstream->first_timestamp = pts;
	    }
	}
	
	vstream->current_receive_count++;
	if (sample_flags) {
	    fprintf(stderr," [INTRA] %ld", vstream->current_receive_count - vstream->last_intra_count);
	    vstream->last_intra_count = vstream->current_receive_count;
	}
	fprintf(stderr,"\n");
    } else if (sample_type == STREAM_TYPE_H264) {
	video_stream_struct *vstream = (video_stream_struct*)core->source_stream[source].video_stream;
	sorted_frame_struct *new_frame;
	uint8_t *new_buffer;
	struct timespec current_time;
	int64_t br;
	int64_t diff;

        if (enable_verbose || sample_flags) {
            fprintf(stderr,"STATUS: RECEIVE_FRAME (H264 :%2d): TYPE:%2d PTS:%15ld DTS:%15ld KEY:%d\n",
                    source,
                    sample_type,
                    pts,
                    dts,
                    sample_flags);
        }   

	if (vstream->total_video_bytes == 0) {
	    clock_gettime(CLOCK_REALTIME, &vstream->video_clock_start);
	}
	vstream->total_video_bytes += sample_size;
	clock_gettime(CLOCK_REALTIME, &current_time);

	diff = time_difference(&current_time, &vstream->video_clock_start) / 1000;
	if (diff > 0) {
	    br = (vstream->total_video_bytes * 8) / diff;
	    vstream->video_bitrate = br * 1000;
	}
	
	if (!vstream->found_key_frame && sample_flags) {
	    vstream->found_key_frame = 1;
	    if (dts > 0) {
		vstream->first_timestamp = dts;
	    } else {
		vstream->first_timestamp = pts;
	    }
	}

	if (dts > 0) {
	    if (vstream->last_timestamp_dts != -1) {	    
		int64_t delta_dts = dts + vstream->overflow_dts - vstream->last_timestamp_dts;
		int64_t mod_overflow = vstream->last_timestamp_dts % 8589934592;
		if (delta_dts < -OVERFLOW_DTS && mod_overflow > OVERFLOW_DTS) {
		    vstream->overflow_dts += 8589934592;
		    fprintf(stderr,"FILLET: DELTA_DTS:%ld OVERFLOW DTS:%ld\n", delta_dts, vstream->overflow_dts);
		    syslog(LOG_INFO,"(%d) VIDEO STREAM TIMESTAMP OVERFLOW - DELTA:%ld  OVERFLOW DTS:%ld  LAST:%ld  DTS:%ld\n",
			   source,
			   delta_dts,
			   vstream->overflow_dts,
			   vstream->last_timestamp_dts,
			   dts);
		} else if (delta_dts < 0 || delta_dts > 60000) {
		    fprintf(stderr,"FILLET: DELTA_DTS:%ld OVERFLOW DTS:%ld (SIGNAL DISCONTINUITY)\n", delta_dts, vstream->overflow_dts);
		    syslog(LOG_INFO,"(%d) VIDEO STREAM TIMESTAMP OVERFLOW - DELTA:%ld  OVERFLOW DTS:%ld  LAST:%ld  DTS:%ld (SIGNAL DISCONTINUITY)\n",
			   source,
			   delta_dts,
			   vstream->overflow_dts,
			   vstream->last_timestamp_dts,
			   dts);
                    fprintf(stderr,"(%d) VIDEO STREAM TIMESTAMP OVERFLOW - DELTA:%ld  OVERFLOW DTS:%ld  LAST:%ld  DTS:%ld (SIGNAL DISCONTINUITY)\n",
                            source,
                            delta_dts,
                            vstream->overflow_dts,
                            vstream->last_timestamp_dts,
                            dts);
		    fprintf(stderr,"FILLET: (%d) RESTARTING SYNC THREAD\n", source);
		    restart_sync_thread = 1;
		}
	    }

	    vstream->last_timestamp_dts = dts + vstream->overflow_dts;
	}
	vstream->last_timestamp_pts = pts + vstream->overflow_dts;
	vstream->current_receive_count++;
	if (sample_flags) {
	    vstream->last_intra_count = vstream->current_receive_count;
	}

	if (!vstream->found_key_frame) {
	    return 0;
	}

	new_buffer = (uint8_t*)malloc(sample_size+1);
	memcpy(new_buffer, sample, sample_size);
	
	new_frame = (sorted_frame_struct*)malloc(sizeof(sorted_frame_struct));
	new_frame->buffer = new_buffer;
	new_frame->buffer_size = sample_size;
	new_frame->pts = pts;
	new_frame->dts = dts;
	if (dts > 0) {
	    new_frame->full_time = dts + vstream->overflow_dts;
	} else {
	    new_frame->full_time = pts + vstream->overflow_dts;
	}
	new_frame->duration = new_frame->full_time - vstream->last_full_time;	
	vstream->last_full_time = new_frame->full_time;
	new_frame->first_timestamp = vstream->first_timestamp;
	new_frame->source = source;
        new_frame->sub_stream = 0;
	new_frame->sync_frame = sample_flags;
	new_frame->frame_type = FRAME_TYPE_VIDEO;
	new_frame->media_type = MEDIA_TYPE_H264;
	new_frame->time_received = 0;
	if (lang_tag) {
	    new_frame->lang_tag[0] = lang_tag[0];
	    new_frame->lang_tag[1] = lang_tag[1];
	    new_frame->lang_tag[2] = lang_tag[2];
	    new_frame->lang_tag[3] = lang_tag[3];	    
	} else {
	    memset(new_frame->lang_tag,0,sizeof(new_frame->lang_tag));
	}
	pthread_mutex_lock(&sync_lock);	
	video_synchronizer_entries = add_frame(core->video_frame_data, video_synchronizer_entries, new_frame, MAX_FRAME_DATA_SYNC_VIDEO);
	if (video_synchronizer_entries >= MAX_FRAME_DATA_SYNC_VIDEO) {
	    restart_sync_thread = 1;	    	   
	}
	pthread_mutex_unlock(&sync_lock);	
    } else if (sample_type == STREAM_TYPE_AAC || sample_type == STREAM_TYPE_AC3) {
	audio_stream_struct *astream = (audio_stream_struct*)core->source_stream[source].audio_stream[sub_source];
	video_stream_struct *vstream = (video_stream_struct*)core->source_stream[source].video_stream;
	uint8_t *new_buffer;
	sorted_frame_struct *new_frame;	
	struct timespec current_time;
	int64_t br;
	int64_t diff;

        /*
        if (sample_type == STREAM_TYPE_AC3) {
            syslog(LOG_INFO,"SESSION:%d (MAIN) ERROR: UNSUPPORTED MEDIA TYPE (AC3) - PLEASE SUBMIT FEATURE REQUEST\n",
                   core->session_id);
            fprintf(stderr,"SESSION:%d (MAIN) ERROR: UNSUPPORTED MEDIA TYPE (AC3) - PLEASE SUBMIT FEATURE REQUEST\n",
                    core->session_id);
            exit(0);
        }
        */

        if (enable_verbose) {
            fprintf(stderr,"STATUS: RECEIVE_FRAME (AUDIO:%2d): TYPE:%2d PTS:%15ld DTS:%15ld KEY:%d (AUDIO STREAM:%d)\n",
                    source,
                    sample_type,
                    pts,
                    dts,
                    vstream->found_key_frame,
                    sub_source);
        }

        if (!vstream->found_key_frame) {
	    return 0;
	}

	if (astream->total_audio_bytes == 0) {
	    clock_gettime(CLOCK_REALTIME, &astream->audio_clock_start);
	}
	astream->total_audio_bytes += sample_size;
	clock_gettime(CLOCK_REALTIME, &current_time);

	diff = time_difference(&current_time, &astream->audio_clock_start) / 1000;
	if (diff > 0) {
	    br = (astream->total_audio_bytes * 8) / diff;
	    astream->audio_bitrate = br * 1000;
	}

	new_buffer = (uint8_t*)malloc(sample_size+1);
	memcpy(new_buffer, sample, sample_size);
	
	if (astream->last_timestamp_pts != -1) {
	    int64_t delta_pts = pts + astream->overflow_pts - astream->last_timestamp_pts;
	    int64_t mod_overflow = astream->last_timestamp_pts % 8589934592;
	    if (delta_pts < -OVERFLOW_DTS && mod_overflow > OVERFLOW_DTS) {	    
		astream->overflow_pts += 8589934592;
		fprintf(stderr,"FILLET: DELTA_PTS:%ld OVERFLOW PTS:%ld\n", delta_pts, astream->overflow_pts);
		syslog(LOG_INFO,"(%d) AUDIO STREAM TIMESTAMP OVERFLOW - DELTA:%ld  OVERFLOW PTS:%ld  PTS:%ld\n",
		       source,
		       delta_pts,
		       astream->overflow_pts,
		       pts);
	    } else if (delta_pts < 0 || delta_pts > 60000) {
		fprintf(stderr,"FILLET: DELTA_PTS:%ld OVERFLOW PTS:%ld\n", delta_pts, astream->overflow_pts);
		syslog(LOG_INFO,"(%d) AUDIO STREAM TIMESTAMP OVERFLOW - DELTA:%ld  OVERFLOW PTS:%ld  PTS:%ld  LAST PTS:%ld\n",
		       source,
		       delta_pts,
		       astream->overflow_pts,
		       pts,
                       astream->last_timestamp_pts);
		fprintf(stderr,"(%d) AUDIO STREAM TIMESTAMP OVERFLOW - DELTA:%ld  OVERFLOW PTS:%ld  PTS:%ld  LAST PTS:%ld\n",
                        source,
                        delta_pts,
                        astream->overflow_pts,
                        pts,
                        astream->last_timestamp_pts);

		fprintf(stderr,"FILLET: (%d) RESTARTING SYNC THREAD\n", source);
		restart_sync_thread = 1;
	    } 	    
	}   	

	astream->last_timestamp_pts = pts + astream->overflow_pts;
	
	new_frame = (sorted_frame_struct*)malloc(sizeof(sorted_frame_struct));

	new_frame->buffer = new_buffer;
	new_frame->buffer_size = sample_size;
	new_frame->pts = pts;
	new_frame->dts = dts;
	new_frame->full_time = pts + astream->overflow_pts;
	new_frame->duration = new_frame->full_time - astream->last_full_time;
	astream->last_full_time = new_frame->full_time;
	new_frame->first_timestamp = 0;
	new_frame->source = source;
        new_frame->sub_stream = sub_source;
	new_frame->sync_frame = sample_flags;
	new_frame->frame_type = FRAME_TYPE_AUDIO;
        if (sample_type == STREAM_TYPE_AAC) {
            new_frame->media_type = MEDIA_TYPE_AAC;
        } else {
            new_frame->media_type = MEDIA_TYPE_AC3;
        }
	new_frame->time_received = 0;
	if (lang_tag) {
	    new_frame->lang_tag[0] = lang_tag[0];
	    new_frame->lang_tag[1] = lang_tag[1];
	    new_frame->lang_tag[2] = lang_tag[2];
	    new_frame->lang_tag[3] = lang_tag[3];	    
	} else {
	    memset(new_frame->lang_tag,0,sizeof(new_frame->lang_tag));
	}	
	pthread_mutex_lock(&sync_lock);	
	audio_synchronizer_entries = add_frame(core->audio_frame_data, audio_synchronizer_entries, new_frame, MAX_FRAME_DATA_SYNC_AUDIO);
	if (audio_synchronizer_entries >= MAX_FRAME_DATA_SYNC_AUDIO) {
	    restart_sync_thread = 1;
	}
	pthread_mutex_unlock(&sync_lock);	
    } else {
	fprintf(stderr,"UNKNOWN SAMPLE RECEIVED\n");
    }

    if (restart_sync_thread) {
        fprintf(stderr,"GETTING SYNC LOCK\n");
	pthread_mutex_lock(&sync_lock);
	dump_frames(core->video_frame_data, MAX_FRAME_DATA_SYNC_VIDEO);
	dump_frames(core->audio_frame_data, MAX_FRAME_DATA_SYNC_AUDIO);        
	pthread_mutex_unlock(&sync_lock);
        fprintf(stderr,"DONE WITH SYNC LOCK\n");
	video_synchronizer_entries = 0;
	audio_synchronizer_entries = 0;
	quit_sync_thread = 1;
        fprintf(stderr,"WAITING FOR SYNC THREAD TO STOP\n");      
	pthread_join(frame_sync_thread_id, NULL);
        fprintf(stderr,"DONE WAITING FOR SYNC THREAD TO STOP\n");
    }
    return 0;
}

static void disable_child_signal()
{
    struct sigaction dowhat;
    
    dowhat.sa_handler = SIG_IGN;
    dowhat.sa_flags = 0;
    sigemptyset(&dowhat.sa_mask);
    sigaction(SIGCHLD, &dowhat, NULL);    
}

static void *runtime_thread(void *context)
{
    fillet_app_struct *core = (fillet_app_struct*)context;
    int i;
    
    register_message_callback(message_dispatch, (void*)core);
    register_frame_callback(receive_frame, (void*)core);

    hlsmux_create(core);
    for (i = 0; i < config_data.active_sources; i++) {
        struct in_addr addr;	     
        snprintf(core->fillet_input[i].interface,UDP_MAX_IFNAME-1,"%s",config_data.active_interface);
        snprintf(core->fillet_input[i].udp_source_ipaddr,UDP_MAX_IFNAME-1,"%s",config_data.active_source[i].active_ip);
        if (inet_aton(config_data.active_source[i].active_ip, &addr) == 0) {
            syslog(LOG_INFO,"SESSION:%d (MAIN) ERROR: INVALID IP ADDRESS: %s\n",
                   core->session_id,
                   config_data.active_source[i].active_ip);
            hlsmux_destroy(core->hlsmux);
            core->hlsmux = NULL;
            return NULL;
        }
        core->fillet_input[i].udp_source_port = config_data.active_source[i].active_port;
    }

    syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: STARTING CORE FRAME SYNC THREAD: ACTIVE SOURCES:%d\n",
           core->session_id,
           config_data.active_sources);

    sync_thread_running = 1;
    pthread_create(&frame_sync_thread_id, NULL, frame_sync_thread, (void*)core);
    core->source_running = 1;    
    for (i = 0; i < config_data.active_sources; i++) {
        syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: STARTING UDP SOURCE THREAD: %d (%s:%d:%s)\n",
               core->session_id,
               i,
               core->fillet_input[i].udp_source_ipaddr,
               core->fillet_input[i].udp_source_port,
               core->fillet_input[i].interface);
        pthread_create(&core->source_stream[i].udp_source_thread_id, NULL, udp_source_thread, (void*)core);
        syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: DONE STARTING UDP SOURCE THREAD: %d (%s:%d:%s)\n",
               core->session_id,
               i,
               core->fillet_input[i].udp_source_ipaddr,
               core->fillet_input[i].udp_source_port,
               core->fillet_input[i].interface);               
    }

    syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: ENTERING CORE FRAME SYNC THREAD\n",
           core->session_id);
    
    while (core->source_running) {
        if (quit_sync_thread && core->source_running) {
            fprintf(stderr,"STATUS: RESTARTING FRAME SYNC THREAD\n");
            while (sync_thread_running) {
                usleep(10000);
                // bail out and respawn if it never comes back
                // waiting to trigger to 0
            }
            //quit_sync_thread = 0;            
            sync_thread_running = 1;
            pthread_create(&frame_sync_thread_id, NULL, frame_sync_thread, (void*)core);
        }
        sleep(1);
    }

    quit_sync_thread = 1;
    syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: STOPPING FRAME SYNC THREAD\n", core->session_id);
    pthread_join(frame_sync_thread_id, NULL);
    sync_thread_running = 0;

    for (i = 0; i < config_data.active_sources; i++) {
        syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: STOPPING UDP SOURCE THREAD: %d (%s:%d:%s)\n",
               core->session_id,
               i,
               core->fillet_input[i].udp_source_ipaddr,
               core->fillet_input[i].udp_source_port,
               core->fillet_input[i].interface);              
        pthread_join(core->source_stream[i].udp_source_thread_id, NULL);
    }
    
    hlsmux_destroy(core->hlsmux);
    core->hlsmux = NULL;
    quit_sync_thread = 0;

    return NULL;
}

int main(int argc, char **argv)
{
     int ret;
     int i;
     fillet_app_struct *core;
     pthread_t server_thread_id;
     pthread_t client_thread_id;
     pthread_t runtime_thread_id;

     signal(SIGSEGV, crash_handler);

     config_data.window_size = DEFAULT_WINDOW_SIZE;
     config_data.segment_length = DEFAULT_SEGMENT_LENGTH;
     config_data.rollover_size = MAX_ROLLOVER_SIZE;
     config_data.active_sources = 0;
     config_data.identity = 1000;
     config_data.enable_ts_output = 1;
     config_data.enable_fmp4_output = 1;
     
     sprintf(config_data.manifest_directory,"/var/www/hls/");
     
     ret = parse_input_options(argc, argv);
     if ((ret < 0 || config_data.active_sources == 0) && (enable_background == 0)) {
         fprintf(stderr,"\n");
	 fprintf(stderr,"fillet is a live packaging service/application for IP based OTT content redistribution\n");
	 fprintf(stderr,"\n");
	 fprintf(stderr,"usage: fillet [options]\n");
	 fprintf(stderr,"\n\n");	  
	 fprintf(stderr,"       --sources    [NUMBER OF ABR SOURCES - MUST BE >= 1 && <= 10]\n");
	 fprintf(stderr,"       --ip         [IP:PORT,IP:PORT,etc.] (Please make sure this matches the number of sources)\n");
	 fprintf(stderr,"       --interface  [SOURCE INTERFACE - lo,eth0,eth1,eth2,eth3]\n");
	 fprintf(stderr,"                    If multicast, make sure route is in place\n\n");
	 fprintf(stderr,"       --window     [WINDOW IN SEGMENTS FOR MANIFEST]\n");
	 fprintf(stderr,"       --segment    [SEGMENT LENGTH IN SECONDS]\n");
	 fprintf(stderr,"       --manifest   [MANIFEST DIRECTORY \"/var/www/hls/\"]\n");
	 fprintf(stderr,"       --identity   [RUNTIME IDENTITY - any number, but must be unique across multiple instances of fillet]\n");
	 fprintf(stderr,"       --background [RUN IN BACKGROUND]\n");
	 fprintf(stderr,"\n");
	 return 1;
     }

     if (enable_background) {
         core = create_fillet_core(&config_data, MAX_SOURCES); // need to reserve ahead of time
     } else {
         core = create_fillet_core(&config_data, config_data.active_sources);
     }
     
     if (enable_background) {
	 int daemon_launched;
	 FILE *pid_vartmp;
	 int child_process;
	 pid_t new_pid;

         fprintf(stderr,"\n");
         fprintf(stderr,"fillet is a live packaging service/application for IP based OTT content redistribution\n");
         fprintf(stderr,"launching in background mode\n");
         fprintf(stderr,"\n");

	 daemon_launched = daemon(0, 1);  // need to see some of the stderr messages
	 if (daemon_launched < 0) {
	     fprintf(stderr,"FILLET: ERROR: Unable to launch application in background!\n\n");
	     return -1;
	 }

         fprintf(stderr,"saving process pid information in /var/tmp/fillet.background\n");
	 pid_vartmp = fopen("/var/tmp/fillet.background","w");
	 if (!pid_vartmp) {
	     fprintf(stderr,"FILLET: ERROR: Unable to open /var/tmp/fillet.background!\n\n");
	     return -1;
	 }
	 fprintf(pid_vartmp,"%u\n", getpid());
	 fclose(pid_vartmp);

         fprintf(stderr,"launching %d background processes\n", MAX_SESSIONS);
	 for (child_process = 0; child_process < MAX_SESSIONS; child_process++) {
	     new_pid = launch_new_fillet(core, child_process);
	     if (new_pid < 0) {
		 fprintf(stderr,"ERROR: Unable to launch new process in background!\n\n");
                 syslog(LOG_INFO,"FILLET: ERROR: Unable to launch new process in background!\n");
		 exit(1);
	     } else if (new_pid != 0) {
                 // parent
                 fprintf(stderr,"STATUS: Launching background worker process: %d (PID:%d)\n", child_process, new_pid);
                 syslog(LOG_INFO,"FILLET: STATUS: PARENT: Launching background worker process: %d (PID:%d)\n",
                        child_process,
                        new_pid);                 
                 child_pids[child_process] = new_pid;
		 continue;
	     } else {
                 // new child
		 syslog(LOG_INFO,"FILLET: STATUS: CHILD: Launched new child process!\n\n");
		 break;
             }
	 }

	 if (new_pid > 0) {
	     //server side

	     disable_child_signal();

	     syslog(LOG_INFO,"FILLET: STATUS: STARTING SERVER THREAD\n");
	     
	     while (1) {
                 int err;
		 //just ping each client asking for health
		 //also check that the process is still alive otherwise refork
                 //fprintf(stderr,"Parent is sleeping\n");
                 for (child_process = 0; child_process < MAX_SESSIONS; child_process++) {
                     //fprintf(stderr,"PID:%d CHECKING IF PROCESS EXISTS: (%d) PID:%d\n", new_pid, child_process, child_pids[child_process]);
                     // checking to see if process exists - setting kill to 0 is simply a check that it exists and can receive signals
                     err = kill(child_pids[child_process], 0);
                     if (err == 0) {
                         // process exists - we are fine
                         continue;
                     }
                     if (err == -1) {
                         syslog(LOG_INFO,"ERROR: PROCESS %d HAS EXITED - STARTING A NEW ONE\n", child_pids[child_process]);
                         fprintf(stderr,"STATUS: PROCESS %d HAS EXITED - STARTING A NEW ONE\n", child_pids[child_process]);

                         new_pid = launch_new_fillet(core, child_process);
                         if (new_pid < 0) {
                             fprintf(stderr,"ERROR: Unable to launch new process in background!\n\n");
                             syslog(LOG_INFO,"FILLET: ERROR: Unable to launch new process in background!\n");
                             exit(1);
                         } else if (new_pid != 0) {
                             fprintf(stderr,"STATUS: Launching background worker process: %d (PID:%d)\n", child_process, new_pid);
                             syslog(LOG_INFO,"FILLET: STATUS: PARENT: Launching background worker process: %d (PID:%d)\n",
                                    child_process,
                                    new_pid);                 
                             child_pids[child_process] = new_pid;
                             continue;
                         } else {
                             // new child
                             syslog(LOG_INFO,"FILLET: STATUS: CHILD: Launched new child process!\n\n");
                             goto _start_child_process;
                         }             
                     }
                 }
		 sleep(1);
	     }

             // this will fall through and the channel will start again
             
	     // wait for the kill signal
             // we should probably put in a master kill switch that gets us here
	     // kill the children
	     //for (child_process = 0; child_process < MAX_SESSIONS; child_process++) {
             // kill(child_pids[child_process], SIGKILL);
	     //}
	     // kill the server
	     //exit(0);
	 }
     } else {
	 // basic command line mode for testing purposes
	 core->session_id = 1;
	 register_message_callback(message_dispatch, (void*)core);
	 register_frame_callback(receive_frame, (void*)core);
	 hlsmux_create(core);
	 for (i = 0; i < config_data.active_sources; i++) {
	     struct in_addr addr;	     
	     snprintf(core->fillet_input[i].interface,UDP_MAX_IFNAME-1,"%s",config_data.active_interface);
	     snprintf(core->fillet_input[i].udp_source_ipaddr,UDP_MAX_IFNAME-1,"%s",config_data.active_source[i].active_ip);
	     if (inet_aton(config_data.active_source[i].active_ip, &addr) == 0) {
		 fprintf(stderr,"\nERROR: INVALID IP ADDRESS: %s\n\n",
			 config_data.active_source[i].active_ip);
		 goto cleanup_main_app;
		 return -1;
	     }
	     core->fillet_input[i].udp_source_port = config_data.active_source[i].active_port;
	 }

         sync_thread_running = 1;
	 pthread_create(&frame_sync_thread_id, NULL, frame_sync_thread, (void*)core);  
	 core->source_running = 1;
	 for (i = 0; i < config_data.active_sources; i++) {	
	     pthread_create(&core->source_stream[i].udp_source_thread_id, NULL, udp_source_thread, (void*)core);
         }

	 while (core->source_running) {
	     if (quit_sync_thread) {
                 while (sync_thread_running) {
                     usleep(10000);
                 }
		 quit_sync_thread = 0;                 
		 fprintf(stderr,"STATUS: RESTARTING FRAME SYNC THREAD\n");
                 sync_thread_running = 1;
		 pthread_create(&frame_sync_thread_id, NULL, frame_sync_thread, (void*)core);
	     }
             usleep(100000);
	 }
	 
cleanup_main_app:
	 fprintf(stderr,"STATUS: LEAVING APPLICATION\n");
	 hlsmux_destroy(core->hlsmux);
         core->hlsmux = NULL;
	 destroy_fillet_core(core);

	 return 0;
     }

_start_child_process:     
     // this is the main child background thread
     syslog(LOG_INFO,"FILLET: STATUS: STARTING CHILD SERVER THREAD: %d\n", core->session_id);

     pthread_create(&client_thread_id, NULL, client_thread, (void*)core);
     while (1) {	 
	 int msgid;

         syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: WAITING FOR EVENT: ACTIVE:%d\n",
                core->session_id,
                core->source_running);
         
	 msgid = wait_for_event(core);
         if (msgid == -1) {
             //placeholder - this is the kill
             break;
         }
         syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: DONE WAITING FOR EVENT - DISPATCH:0x%x\n",
                core->session_id,
                msgid);
         if (msgid == MSG_START) {
             int err;
             
             if (core->source_running) {
                 syslog(LOG_INFO,"SESSION: %d (MAIN) WARNING: CORE SESSION ALREADY RUNNING\n", core->session_id);
                 // nope!
                 continue;
             }
             //reload configuration file when starting
             err = load_kvp_config(core);
             if (err < 0) {
                 syslog(LOG_INFO,"SESSION:%d (MAIN) ERROR: UNABLE TO PROCESS CONFIGURATION\n", core->session_id);
                 continue;
             }
             syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: STARTING CORE SESSION\n", core->session_id);
             pthread_create(&runtime_thread_id, NULL, runtime_thread, (void*)core);
         } else if (msgid == MSG_STOP) {
             if (!core->source_running) {
                 syslog(LOG_INFO,"SESSION: %d (MAIN) WARNING: CORE SESSION NOT RUNNING\n", core->session_id);
                 // nope!
                 continue;
             }
             syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: STOPPING CORE SESSION\n", core->session_id);
             core->source_running = 0;
             pthread_join(runtime_thread_id, NULL);
             syslog(LOG_INFO,"SESSION:%d (MAIN) STATUS: CORE SESSION STOPPED\n", core->session_id);             
         } else if (msgid == MSG_RESTART) {
             if (!core->source_running) {
                 // nope!
                 continue;
             }
             core->source_running = 0;
             pthread_join(runtime_thread_id, NULL);
             pthread_create(&runtime_thread_id, NULL, runtime_thread, (void*)core);
         } else if (msgid == MSG_RESPAWN) {
             exit(0);  // force respawn
         } 
     }
     pthread_join(client_thread_id, NULL);
     destroy_fillet_core(core);     
     
     return 0;
}
