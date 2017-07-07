/******************************************************************************
* battery_monitor.c
* James Strawson - 2016
*
* see README.txt for details
*******************************************************************************/

#include "../../libraries/roboticscape-usefulincludes.h"
#include "../../libraries/roboticscape.h"
#include "../../libraries/roboticscape-defs.h"
#include "../../libraries/mmap/mmap_gpio_adc.h"
#include <sys/file.h>

#define BATTPIDFILE	"/var/run/battery_monitor.pid"

// Critical Max voltages of packs used to detect number of cells in pack
#define CELL_MAX		4.25 // set higher than actual to detect num cells
#define CELL_FULL		3.90 // minimum V to consider battery full
#define CELL_75			3.80 // minimum V to consider battery 75%
#define CELL_50			3.60 // minimum V to consider battery 50%
#define CELL_25			3.25 // minimum V to consider battery 25%
#define CELL_DIS		2.70 // Threshold for detecting disconnected battery
#define V_CHG_DETECT  	4.15 // above this assume finished charging

// filter
#define LOOP_SLEEP_US 		300000 	// 2hz sample
#define FITLER_SAMPLES 		6		// average over 6 samples, 3 seconds
#define STD_DEV_TOLERANCE 	0.04 	// above 0.1 definitely charging

// functions
void illuminate_leds(int i);
int kill_existing_instance();
void shutdown_signal_handler(int signo);

// gpio designation for led 2 is a global variable
// the rest are #defines but since led 2 is different on the blue
// we must make it variable
int batt_led_2;
int running;


// main() takes only one optional argument: -k (kill)
int main(int argc, char *argv[]){
	FILE* fd;
	float v_pack;	// 2S pack voltage on JST XH 2S balance connector
	float v_jack;	// could be dc power supply or another battery
	float cell_voltage;	// cell voltage from either 2S or external pack
	int toggle = 0;
	int printing = 0;
	int num_cells = 0;
	int chg_leds = 0;
	int charging = 0;
	int pack_connected = 0;
	int c, i;
	float stddev;
	d_filter_t filterB, filterJ; // battery and jack filters

	// parse arguments to check for kill mode
	opterr = 0;
	while ((c = getopt(argc, argv, "k")) != -1){
		switch (c){
		case 'k':  // kill mode
			return kill_existing_instance();
			break;
			
		default:
			printf("\nInvalid Argument \n");
			return -1;
			break;
		}
    }

	// we only want one instance running, so check is a pid file already exists
	if(access(PID_FILE, F_OK ) == 0){
		// PID file exists
		printf("instance of battery_monitor already running\n");
		return -1;
	}

	// make new pid file
	fd = fopen(BATTPIDFILE, "ab");
	if (fd < 0) {
		printf("\n error opening PID file for writing\n");
		return -1;
	}
	pid_t current_pid = getpid();
	fprintf(fd,"%d",(int)current_pid);
	fflush(fd);
	fclose(fd);

	// set up signal handler
    signal(SIGINT, shutdown_signal_handler);	
	signal(SIGTERM, shutdown_signal_handler);	

	// set led 2 gpio designation depending on board
	if(get_bb_model()==BB_BLUE) batt_led_2=BATT_LED_2_BLUE;
	else batt_led_2=BATT_LED_2;

	// open the gpio channels for 4 battery indicator LEDs
	gpio_export(BATT_LED_1);
	gpio_export(batt_led_2);
	gpio_export(BATT_LED_3);
	gpio_export(BATT_LED_4);
	gpio_set_dir(BATT_LED_1, OUTPUT_PIN);
	gpio_set_dir(batt_led_2, OUTPUT_PIN);
	gpio_set_dir(BATT_LED_3, OUTPUT_PIN);
	gpio_set_dir(BATT_LED_4, OUTPUT_PIN);
	
	// enable adc
	initialize_mmap_adc();
	initialize_mmap_gpio();

	// start filters
	v_pack = get_battery_voltage();
	filterB = create_moving_average(FITLER_SAMPLES);
	prefill_filter_outputs(&filterB, v_pack);
	prefill_filter_inputs(&filterB, v_pack);
	v_jack = get_dc_jack_voltage();
	filterJ = create_moving_average(FITLER_SAMPLES);
	prefill_filter_outputs(&filterJ, v_jack);
	prefill_filter_inputs(&filterJ, v_jack);

	// vector for standard deviation check
	vector_t vec = create_vector(FITLER_SAMPLES);
		
	
	// first decide if the user has called this from a terminal
	// or as a startup process
	if(isatty(fileno(stdout))){
		printing = 1;
		printf("\n2S Pack   Jack   #Cells   Cell\n");
	}
	
	// run intil running==0 which is set by signal handler
	running = 1;
	while(running){
		charging = 0;
		// read in the voltage of the 2S pack and DC jack
		v_pack = march_filter(&filterB, get_battery_voltage());
		v_jack = march_filter(&filterJ, get_dc_jack_voltage());

		if(v_pack==-1 || v_jack==-1){
			printf("can't read ADC voltages\n");
			return -1;
		}
		
		// find standard deviation of battery signal to determine
		// if a 2S pack is connected or not
		if(v_pack>(2*CELL_DIS)){
			for(i=0; i<FITLER_SAMPLES; i++){
				vec.data[i]=filterB.in_buf.data[i];
			}
			stddev=standard_deviation(vec);
			//printf("stddev: %f\n", stddev);
		}

		// check if 2s pack if connected
		if(v_pack>(2*CELL_DIS) && stddev<STD_DEV_TOLERANCE){
			pack_connected = 1;
			cell_voltage = v_pack/2;
			num_cells = 2;
		}
		else{
			pack_connected = 0;
		}

		// check charging condition
		if(pack_connected && v_jack>10.0 && cell_voltage<V_CHG_DETECT){
			charging = 1;
		}
		else{
			charging = 0;
		}
			
		// no 2S pack on the White 3-pin connector, check for dc jack batteries
		if(!pack_connected){
			// check 4S condition
			if(v_jack>CELL_DIS*4 && v_jack<CELL_MAX*4){
				num_cells = 4;
				cell_voltage = v_jack/4;
			}
			// check for 3S condition
			else if(v_jack>CELL_DIS*3 && v_jack<CELL_MAX*3){
				num_cells = 3;
				cell_voltage = v_jack/3;
			}
			// check for 2S condition
			else if(v_jack>CELL_DIS*2 && v_jack<CELL_MAX*2){ 
				num_cells = 2;
				cell_voltage = v_jack/2;
			}
			// no pack connected
			else {
				cell_voltage = 0;
				num_cells = 0;
			}
		}
		
		// done sensing, start outputting
		if(printing){
			printf("\r %0.2fV   %0.2fV     %d     %0.2fV   ", \
									v_pack, v_jack, num_cells, cell_voltage);
			fflush(stdout);
		}

		// if charging, blink in a charging pattern
		if(charging){
			chg_leds += 1;
			if(chg_leds>4) chg_leds=0;
			illuminate_leds(chg_leds);
		}
		// illuminate LEDs properly if not charging
		else if(num_cells==0) illuminate_leds(0);
		// turn off LEDs if obviously 12v power supply
		else if(num_cells!=2 && v_jack>11.5 && v_jack<12.5){
			illuminate_leds(0);
		}	
		// normal battery discharging
		else if(cell_voltage<CELL_DIS)	illuminate_leds(0);
		else if(cell_voltage>CELL_FULL) illuminate_leds(4);
		else if(cell_voltage>CELL_75) 	illuminate_leds(3);
		else if(cell_voltage>CELL_50) 	illuminate_leds(2);
		else if(cell_voltage>CELL_25) 	illuminate_leds(1);
		// battery is extremely low, blink all 4
		else{
			if(toggle) toggle=0;
			else toggle=4;
			illuminate_leds(toggle);
		}
		

		// sleepy time
		usleep(LOOP_SLEEP_US);
	}

	// exit
	illuminate_leds(0);
	printf("battery_monitor exiting cleanly\n");
	remove(PID_FILE);
	return 0;
}


/*******************************************************************************
* shutdown_signal_handler(int signo)
*
* catch Ctrl-C signal and change running state to 0 indicating exiting
*******************************************************************************/
void shutdown_signal_handler(int signo){
	if (signo == SIGINT){
		running = 0;
 	}else if (signo == SIGTERM){
		running = 0;
		printf("\nreceived SIGTERM\n");
 	}
}



int kill_existing_instance(){
	FILE* fd;
	int old_pid, i;
	
	// attempt to open PID file
	fd = fopen(BATTPIDFILE, "r");
	// if the file didn't open, no proejct is runnning in the background
	// so return 0
	if (fd == NULL) {
		return 0;
	}
	
	// otherwise try to read the current process ID
	fscanf(fd,"%d", &old_pid);
	fclose(fd);
	
	// if the file didn't contain a PID number, remove it and 
	// return -1 indicating weird behavior
	if(old_pid == 0){
		remove(BATTPIDFILE);
		return 1;
	}
		
	// attempt a clean shutdown
	kill((pid_t)old_pid, SIGINT);
	
	// check every 0.1 seconds to see if it closed 
	for(i=0; i<20; i++){
		if(getpgid(old_pid) >= 0) usleep(100000);
		else{ // succcess, it shut down properly
			remove(BATTPIDFILE);
			return 1; 
		}
	}
	
	// otherwise force kill the program if the PID file never got cleaned up
	kill((pid_t)old_pid, SIGKILL);

	// close and delete the old file
	remove(BATTPIDFILE);
	
	// return -1 indicating the program had to be killed
	return -1;
}



void illuminate_leds(int i){
	switch(i){
	// now illuminate LEDs properly
	case 4:
		mmap_gpio_write(BATT_LED_1,HIGH);
		mmap_gpio_write(batt_led_2,HIGH);
		mmap_gpio_write(BATT_LED_3,HIGH);
		mmap_gpio_write(BATT_LED_4,HIGH);
		break;
	case 3:
		mmap_gpio_write(BATT_LED_1,HIGH);
		mmap_gpio_write(batt_led_2,HIGH);
		mmap_gpio_write(BATT_LED_3,HIGH);
		mmap_gpio_write(BATT_LED_4,LOW);
		break;
	case 2:
		mmap_gpio_write(BATT_LED_1,HIGH);
		mmap_gpio_write(batt_led_2,HIGH);
		mmap_gpio_write(BATT_LED_3,LOW);
		mmap_gpio_write(BATT_LED_4,LOW);
		break;
	case 1:
		mmap_gpio_write(BATT_LED_1,HIGH);
		mmap_gpio_write(batt_led_2,LOW);
		mmap_gpio_write(BATT_LED_3,LOW);
		mmap_gpio_write(BATT_LED_4,LOW);
		break;
	case 0:
		mmap_gpio_write(BATT_LED_1,LOW);
		mmap_gpio_write(batt_led_2,LOW);
		mmap_gpio_write(BATT_LED_3,LOW);
		mmap_gpio_write(BATT_LED_4,LOW);
		break;
	default:
		printf("can only illuminate between 0 and 4 leds\n");
		printf("attempting %d\n", i);
		break;
	}
	return;
}


