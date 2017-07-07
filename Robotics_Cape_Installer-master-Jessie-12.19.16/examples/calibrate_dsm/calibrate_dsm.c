/*******************************************************************************
* calibrate_dsm2.c
*
* James Strawson 2016
* Running the calibrate_dsm2 example will print out raw data to the console and 
* record the min and max values for each channel. These limits will be saved to
* disk so future dsm2 reads will be scaled correctly.
* 
* Make sure the transmitter and receiver are paired before testing. 
* Use the pair_dsm2 example if you haven't already used a bind plug and standard 
* receiver to pair. The satellite receiver remembers which transmitter it is 
* paired to, not your BeagleBone.
*******************************************************************************/

#include "../../libraries/roboticscape-usefulincludes.h"
#include "../../libraries/roboticscape.h"

int main(){
	if(initialize_cape()<0){
		printf("ERROR: failed to initialize_cape\n");
	}
	
	printf("Please connect a DSM sattelite reciever to your Robotics Cape\n");
	printf("and make sure your transmitter is on and paired to the receiver\n");
	printf("\n");
	printf("Press ENTER to continue or anything else to quit\n");
	if(continue_or_quit()<1){
		cleanup_cape();
		return -1;
	}
	
	// run the calibration routine
	calibrate_dsm_routine();
	
	// cleanup and close, calibration file already saved by the routine
	cleanup_cape();
	return 0;
}
