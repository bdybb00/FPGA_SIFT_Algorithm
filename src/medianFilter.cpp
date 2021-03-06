#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <math.h>

#include <CL/cl.h>

#include "bitmap.h"
#include "oclHelper.h"
#include "medianFilter.h"
#include "param.h"

#define PIX_PER_KP 10  //pre_allocate buffers for keypoints

/* Global Variables */
const float global_sigmaRatio = pow(2.0, (1.0 / par_Scales));
const int global_shape[2] = {386, 217}; // a size of image which is read from "scipy.misc.imread" function
int global_width, global_height;
int global_cnt;//global_cnt is same as 'self.cnt[0]'
int global_kpsize32;

/* OpenCL Global Variables */
cl_kernel mKernel_local_maxmin, mKernel_interp_keypoint, mKernel_compute_gradient_orientation,\
mKernel_orientation_assignment, mKernel_descriptor;
oclHardware hardware;
oclSoftware software;
cl_mem cl_mem_DoGs, cl_mem_Kp_1, cl_mem_cnt;

/*
 * 	Custom Functions
 */
void checkErrorStatus(cl_int error, const char* message)
{
	if (error != CL_SUCCESS)
	{
		printf("the error detected by checkErrorStatus()\n");
		printf("%s\n", message) ;
		printf("%s\n", oclErrorCode(error)) ;
		exit(0) ;
	}
}

/*
 *  input_type:
 *   It must have space between two arguments.
 *   ex) "%f %f %d %cl"
 */
void setArguments(cl_kernel mKernel, char *input_type, ...) {
	va_list ap;
	cl_int err = 0;
	int num = 0;

	va_start(ap, input_type);
	char *ptr = strtok(input_type, " ");

	while (ptr != NULL)
	{
		if(!strcmp(ptr, "%f")) {
			float *temp = (float *)va_arg(ap, double*);
			err = clSetKernelArg(mKernel, num, sizeof(float), temp);
			checkErrorStatus(err, "Unable to set argument");
		}
		else if(!strcmp(ptr, "%d")) {
			int *temp = (int *)va_arg(ap, int*);
			err = clSetKernelArg(mKernel, num, sizeof(int), temp);
			checkErrorStatus(err, "Unable to set argument");
		}
		else if(!strcmp(ptr, "%cl")) {
			cl_mem *temp = (cl_mem *)va_arg(ap, cl_mem*);
			err = clSetKernelArg(mKernel, num, sizeof(cl_mem), temp);
			checkErrorStatus(err, "Unable to set argument");
		}
		ptr = strtok(NULL, " ");
		num += 1;
	}

	va_end(ap);
}

void OpenCL_Initialize (const char *target_device_name, const char *xclbinFilename) {
	// Set up OpenCL hardware and software constructs
	std::cout << "Setting up OpenCL hardware and software...\n";
	cl_int err = 0;

	hardware = getOclHardware(CL_DEVICE_TYPE_ACCELERATOR, target_device_name) ;
	memset(&software, 0, sizeof(oclSoftware));
	strcpy(software.mFileName, xclbinFilename) ;
	strcpy(software.mCompileOptions, "-g -Wall") ;

	//"get_software()" start
	cl_device_type deviceType = CL_DEVICE_TYPE_DEFAULT;
	err = clGetDeviceInfo(hardware.mDevice, CL_DEVICE_TYPE, sizeof(deviceType), &deviceType, 0);
	if ( err != CL_SUCCESS) {
		std::cout << oclErrorCode(err) << "\n";
		return -1;
	}

	unsigned char *kernelCode = 0;
	std::cout << "Loading " << software.mFileName << "\n";

	int size = loadFile2Memory_global(software.mFileName, (char **) &kernelCode);
	if (size < 0) {
		std::cout << "Failed to load kernel\n";
		return -2;
	}

	if (deviceType == CL_DEVICE_TYPE_ACCELERATOR) {
		size_t n = size;
		software.mProgram = clCreateProgramWithBinary(hardware.mContext, 1, &hardware.mDevice, &n,
				(const unsigned char **) &kernelCode, 0, &err);
	}
	else {
		software.mProgram = clCreateProgramWithSource(hardware.mContext, 1, (const char **)&kernelCode, 0, &err);
	}
	if (!software.mProgram || (err != CL_SUCCESS)) {
		std::cout << oclErrorCode(err) << "\n";
		return -3;
	}

	//Below codes are different by functions
	mKernel_local_maxmin = clCreateKernel(software.mProgram, "local_maxmin", NULL);
	mKernel_interp_keypoint = clCreateKernel(software.mProgram, "interp_keypoint", NULL);
	mKernel_compute_gradient_orientation = clCreateKernel(software.mProgram, "compute_gradient_orientation", NULL);
	mKernel_orientation_assignment = clCreateKernel(software.mProgram, "orientation_assignment", NULL);
	mKernel_descriptor = clCreateKernel(software.mProgram, "descriptor", NULL);

	if (mKernel_local_maxmin == 0 || mKernel_interp_keypoint == 0 || mKernel_compute_gradient_orientation == 0\
			|| mKernel_orientation_assignment == 0 || mKernel_descriptor == 0) {
		std::cout << oclErrorCode(err) << "\n";
		return -4;
	}

	delete [] kernelCode;
	//"get_software()" end

	/* */
	cl_int* counter = new cl_int[1];
	counter[0] = 0;

	cl_mem_cnt = clCreateBuffer(hardware.mContext,
			CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, sizeof(cl_int), counter, &err);
	checkErrorStatus(err, "Unable to create read buffer");

	cl_mem_DoG = clCreateBuffer(hardware.mContext,
			CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, DoGs_size * sizeof(cl_float), DoGs,
			&err);
	checkErrorStatus(err, "Unable to create read buffer");

	cl_mem_Kp_1 = clCreateBuffer(hardware.mContext,
			CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, Kp_1_size * sizeof(cl_float4),
			Kp_1, &err);
	checkErrorStatus(err, "Unable to create write buffer");
}

/*
 *  sift_pyocl, plan.py functions
 */

void __init__(shape, dtype) {

	global_kpsize32 = floor((global_shape[0] * global_shape[1]) / PIX_PER_KP);
}

void _one_octave(int octave) {
	float prevSigma = par_InitSigma, sigma;
	int octsize = pow(2, octave);
	int last_start = 0;
	int scale, newcnt;
	int wgsize2;//??? not 'int'
	int procsize2;

	/* Custom OpenCL local variables */
	int image_size = height*width;
	int DoGs_size = 5 * height * width;
	int Kp_1_size = image_size / PIX_PER_KP;

	size_t globalSize[3] = { 256, 386, 1 } ;
	size_t localSize[3] = { 256, 1, 1} ;
	cl_event seq_complete ;

	for(scale=0; scale<par_Scales+2; scale++) {
		sigma = prevSigma * sqrt(pow(global_sigmaRatio, 2) - 1.0);

		// Calculate gaussian blur and DoG

		//_gaussian_convolution(buffers[scale], buffers[scale + 1], sigma, octave);//??
		prevSigma *= global_sigmaRatio;
		//combine( //???
	}


	for(scale=1; scale<par_Scales+1; scale++) {
		/*
		 * local_maxmin() Kernel function Execution
		 */
		DoGs = new cl_float[DoGs_size];
		temp_string = new char[129];
		strcpy(temp_string, "%cl %cl %d %f %d %f %f %cl %d %d %d %d");
		setArguments(mKernel_local_maxmin, temp_string, \
				&cl_mem_DoGs, &cl_mem_Kp_1, &(int)par_border_dist, &(float)par_peak_thresh, &octsize, &(float)par_EdgeThresh0,\
				&(float)par_EdgeThresh, &cl_mem_cnt, &kpsize32, &scale, &global_width, &global_height);
		delete temp_string;

		err = clEnqueueNDRangeKernel(hardware.mQueue, mKernel_local_maxmin, 2, NULL,\
				globalSize, localSize, 0, NULL, &seq_complete) ;
		checkErrorStatus(err, "Unable to enqueue NDRange") ;
		clWaitForEvents(1, &seq_complete) ;

		/*
		 * interp_keypoint() Kernel function Execution
		 */
		//Set arguments
		err = clEnqueueReadBuffer(hardware.mQueue, cl_mem_cnt,
		CL_TRUE, 0, sizeof(cl_int), global_cnt, 0,
		NULL, &seq_complete);
		checkErrorStatus(err, "Unable to enqueue read buffer");
		clWaitForEvents(1, &seq_complete);
		
		temp_string = new char[129];
		strcpy(temp_string, "%cl %cl %d %d %f %f %d %d");
		setArguments(mKernel_interp_keypoint, temp_string, \
				&cl_mem_DoGs, &cl_mem_Kp_1, &last_start, &global_cnt,\
				&(float)par_peak_thresh, &(float)par_InitSigma, &global_width, &global_height);
		delete temp_string;

		err = clEnqueueNDRangeKernel(hardware.mQueue, mKernel_interp_keypoint, 1, NULL,\
				globalSize, localSize, 0, NULL, &seq_complete) ;
		checkErrorStatus(err, "Unable to enqueue NDRange") ;
		clWaitForEvents(1, &seq_complete) ;

		/*
		 *  self._compact()
		 */
		newcnt = _compact(last_start);

		/*
		 * Rescale all images to populate all octaves
		 */
		/*
		 * compute_gradient_orientation() Kernel function Execution
		 */
		//Write to input buffer
		//??? igray, grad, ori should be initialized.
		//Set arguments
		temp_string = new char[129];
		strcpy(temp_string, "%cl %cl %cl %d %d");
		setArguments(mKernel_compute_gradient_orientation, temp_string, \
				, , ,\//igray, grad, ori
				&global_width, &global_height);

		// Actually start the kernels on the hardware
		std::cout<<"Start the kernels...\n";
		err = clEnqueueNDRangeKernel(hardware.mQueue, mKernel_compute_gradient_orientation, 1, NULL,\
				globalSize, localSize, 0, NULL, &seq_complete) ;
		checkErrorStatus(err, "Unable to enqueue NDRange") ;
		clWaitForEvents(1, &seq_complete) ;

		// Read back the image from the kernel
		/*std::cout << "Reading output image and writing to file...\n";
		  err = clEnqueueReadBuffer(hardware.mQueue, KpFromDevice, CL_TRUE, 0, Kp_1_size,\
						Kp_1, 0, NULL, &seq_complete) ;
		  checkErrorStatus(err, "Unable to enqueue read buffer") ;
		  clWaitForEvents(1, &seq_complete) ;*/

		//Orientation assignement: 1D kernel, rather heavy kernel
		if(newcnt && newcnt > last_start) {
			wgsize2 = global_kernels[file_to_use];//??? plan.py 719
			procsize2 = int(newcnt * wgsize2[0]);

			/*
			 * orientation_assignment() Kernel function Execution
			 */
			//Write to input buffer
			//keypoint, grad, ori, counter should be initialized.
			//Set arguments
			temp_string = new char[129];
			strcpy(temp_string, "%cl %cl %cl %cl %d %f %d %d %d %d %d");
			setArguments(mKernel_orientation_assignment, temp_string, \
					, , , ,\//keypoint, grad, ori, counter
					&octsize, &par_OriSigma, &kpsize32, &last_start, &newcnt, &width, &height);

			// Actually start the kernels on the hardware
			std::cout<<"Start the kernels...\n";
			err = clEnqueueNDRangeKernel(hardware.mQueue, mKernel_orientation_assignment, 1, NULL,\
					globalSize, localSize, 0, NULL, &seq_complete) ;
			checkErrorStatus(err, "Unable to enqueue NDRange") ;
			clWaitForEvents(1, &seq_complete) ;

			// Read back the image from the kernel
			/*std::cout << "Reading output image and writing to file...\n";
			  err = clEnqueueReadBuffer(hardware.mQueue, KpFromDevice, CL_TRUE, 0, Kp_1_size,\
							Kp_1, 0, NULL, &seq_complete) ;
			  checkErrorStatus(err, "Unable to enqueue read buffer") ;
			  clWaitForEvents(1, &seq_complete) ;*/


			//??? plan.py 713, evt_cp = pyopencl.enqueue_copy(self.queue, self.cnt, self.buffers["cnt"].data)
			newcnt = global_cnt[0];

			wgsize2 = global_kernels[file_to_use];//???tuple........
			procsize2 = int(newcnt * wgsize2[0]), wgsize2[1], wgsize2[2];
			try{
				/*
				 * descriptor() Kernel function Execution
				 */
				//Write to input buffer
				//keypoint, descriptor, grad, ori, keypoints_end should be initialized.
				//Set arguments
				temp_string = new char[129];
				strcpy(temp_string, "%cl %cl %cl %cl %d %d %cl %d %d");
				setArguments(mKernel_orientation_assignment, temp_string, \
						, , , ,\//keypoint, descriptor, grad, ori
						&octsize, &last_start, , &width, &height);//keypoints_end

				// Actually start the kernels on the hardware
				std::cout<<"Start the kernels...\n";
				err = clEnqueueNDRangeKernel(hardware.mQueue, mKernel_orientation_assignment, 1, NULL,\
						globalSize, localSize, 0, NULL, &seq_complete) ;
				checkErrorStatus(err, "Unable to enqueue NDRange") ;
				clWaitForEvents(1, &seq_complete) ;

				// Read back the image from the kernel
				/*std::cout << "Reading output image and writing to file...\n";
				  err = clEnqueueReadBuffer(hardware.mQueue, KpFromDevice, CL_TRUE, 0, Kp_1_size,\
								Kp_1, 0, NULL, &seq_complete) ;
				  checkErrorStatus(err, "Unable to enqueue read buffer") ;
				  clWaitForEvents(1, &seq_complete) ;*/
			}
			catch(int e){
				//???743 line~769
			}
			//??? 776 line  evt_cp = pyopencl.enqueue_copy(self.queue, self.cnt, self.buffers["cnt"].data)
			last_start = global_cnt[0];
		}
	}//for() end

	/*
	 * Rescale all images to populate all octaves
	 */
	if(octave < global_octave_max - 1) {
		/*
		 * shrink() Kernel function Execution
		 */
	}
}

/*
 *	Compact the vector of keypoints starting from start
 *
 *  @param start: start compacting at this adress. Before just copy
 *	@type start: numpy.int32
 */
int _compact(int start) {
	int wgsize, kpsize32;
	wgsize = global_max_workgroup_size; //(max(self.wgsize[0]),) #TODO: optimize
	kpsize32 = global_kpsize;
	cp0_evt = pyopencl.enqueue_copy(self.queue, self.cnt, self.buffers["cnt"].data)
			kp_counter = self.cnt[0]
								  procsize = calc_size((self.kpsize,), wgsize)

								  if kp_counter > 0.9 * self.kpsize:
								  logger.warning("Keypoint counter overflow risk: counted %s / %s" % (kp_counter, self.kpsize))
								  logger.info("Compact %s -> %s / %s" % (start, kp_counter, self.kpsize))
								  self.cnt[0] = start
								  cp1_evt = pyopencl.enqueue_copy(self.queue, self.buffers["cnt"].data, self.cnt)
								  evt = self.programs["algebra"].compact(self.queue, procsize, wgsize,
										  self.buffers["Kp_1"].data,  # __global keypoint* keypoints,
										  self.buffers["Kp_2"].data,  # __global keypoint* output,
										  self.buffers["cnt"].data,  # __global int* counter,
										  start,  # int start,
										  kp_counter)  # int nbkeypoints
										  cp2_evt = pyopencl.enqueue_copy(self.queue, self.cnt, self.buffers["cnt"].data)
# swap keypoints:
										  self.buffers["Kp_1"], self.buffers["Kp_2"] = self.buffers["Kp_2"], self.buffers["Kp_1"]
# memset buffer Kp_2
#        self.buffers["Kp_2"].fill(-1, self.queue)
																														  mem_evt = self.programs["memset"].memset_float(self.queue, calc_size((4 * self.kpsize,), wgsize), wgsize, self.buffers["Kp_2"].data, numpy.float32(-1), numpy.int32(4 * self.kpsize))
																														  if self.profile:
																														  self.events += [("copy cnt D->H", cp0_evt),
																																		  ("copy cnt H->D", cp1_evt),
																																		  ("compact", evt),
																																		  ("copy cnt D->H", cp2_evt),
																																		  ("memset 2", mem_evt)
																																		  ]
																																		  znsj
																																		  return self.cnt[0]
}

int main(int argc, char* argv[])
{
	//TARGET_DEVICE macro needs to be passed from gcc command line
#if defined(SDX_PLATFORM) && !defined(TARGET_DEVICE)
#define STR_VALUE(arg)      #arg
#define GET_STRING(name) STR_VALUE(name)
#define TARGET_DEVICE GET_STRING(SDX_PLATFORM)
#endif

	//TARGET_DEVICE macro needs to be passed from gcc command line
	const char *target_device_name = TARGET_DEVICE;
	const char* xclbinFilename = argv[2] ;

	OpenCL_Initialize(target_device_name, xclbinFilename);

	/*
	 * Function Start!!!!!!!!!!
	 */
	// Define Variables

	// Release software and hardware
	release(software) ;
	release(hardware) ;

	return 0 ;
}
