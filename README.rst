.. _polling_server:

Polling Server
###########

Overview
********

This program implements 5 periodic threads out of which one is a polling server which serves requests in an aperiodic fashion(random execution times). THe server runs with a priority of 6(second highest) with a budget of 30ms. As it executes requests its budget is reduced to zero and then it switches to background mode, with a priority of 14. It can also turn into a background server if there are no messages to serve in the message queue.

In the end, we calculate the average response time taken by the polling server when it runs actively with a priority of 6 and when it switches to a background server running with a priority of 14. This gives us an idea about the performance of the server in active v/s background modes. 

Building and Running
********************

This application can be built and executed on NXP MIMXRT1050EVK board as follows:

1. Copy and unzip the RTES-Shankar-R_04.zip in the Zephyrproject workspace, then cd RTES-Shankar-R_04/ from the terminal.
2. Run west build -b mimxrt1050_evk project_4 --pristine .
3. After a successful build, cd /project_4, run west flash.
4. Open putty with the following Serial Configuration:
	port: /dev/ttyACM0
	Baud Rate: 115200
then click Open.
5. In the serial debug console, check the average response time, and the timing diagrams on SystemView.
 	

Sample Output
=============

Check the timing diagram on SystemView. 

The average execution time should be available on putty. 
