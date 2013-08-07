/*
 * Copyright 1993-2009 NVIDIA Corporation.  All rights reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property and 
 * proprietary rights in and to this software and related documentation. 
 * Any use, reproduction, disclosure, or distribution of this software 
 * and related documentation without an express license agreement from
 * NVIDIA Corporation is strictly prohibited.
 *
 * Please refer to the applicable NVIDIA end user license agreement (EULA) 
 * associated with this source code for terms and conditions that govern 
 * your use of this NVIDIA software.
 * 
 */

// *********************************************************************
// Demo application for realtime DXT1 compression using OpenCL
// Based on the C for CUDA DXTC sample
// *********************************************************************

// standard utilities and systems includes
#include <oclUtils.h>

#include "dds.h"
#include "permutations.h"
#include "block.h"

const char *image_filename = "lena.ppm";
const char *refimage_filename = "lena_ref.dds";

unsigned int width, height;
cl_uint* h_img = NULL;

#define GPU_PROFILING

#define ERROR_THRESHOLD 0.02f

#define NUM_THREADS   64      // Number of threads per work group.

#ifdef __EMSCRIPTEN__

#include "SDL/SDL.h"
#include "SDL/SDL_image.h"
#include "SDL/SDL_opengl.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

int hasext(const char *exts, const char *ext) // from cube2, zlib licensed
{
    int len = strlen(ext);
    if(len) for(const char *cur = exts; (cur = strstr(cur, ext)); cur += len)
    {
        if((cur == exts || cur[-1] == ' ') && (cur[len] == ' ' || !cur[len])) return 1;
    }
    return 0;
}

void showtexture(const char * texture_name,int header_size)
{
    SDL_Surface *screen;

    // Slightly different SDL initialization
    if ( SDL_Init(SDL_INIT_VIDEO) != 0 ) {
        printf("Unable to initialize SDL: %s\n", SDL_GetError());
        return;
    }

    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 ); // *new*

    screen = SDL_SetVideoMode( 512, 512, 16, SDL_OPENGL ); // *changed*
    if ( !screen ) {
        printf("Unable to set video mode: %s\n", SDL_GetError());
        return;
    }

    // Check extensions

    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    assert(hasext(exts, "GL_ARB_texture_compression"));
    assert(hasext(exts, "GL_EXT_texture_compression_s3tc"));

    // Set the OpenGL state after creating the context with SDL_SetVideoMode

    glClearColor( 0, 0, 0, 0 );

    glEnable( GL_TEXTURE_2D ); // Needed when we're using the fixed-function pipeline.

    glViewport( 0, 0, 512, 512 );

    glMatrixMode( GL_PROJECTION );
    GLfloat matrixData[] = { 2.0/512,        0,  0,  0,
                                   0, -2.0/512,  0,  0,
                                   0,        0, -1,  0,
                                  -1,        1,  0,  1 };
    glLoadMatrixf(matrixData); // test loadmatrix

    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();


    // Load the OpenGL texture

    GLuint texture;

    FILE *dds = fopen(/*"./data/lena_ref.dds"*/texture_name, "rb");
    
    fseek(dds, 0, SEEK_END); // seek to end of file
    int dds_size = ftell(dds); // get current file pointer
    fseek(dds, 0, SEEK_SET); // seek back to beginning of file
   
    printf("Read %s : Size %d : Header %d\n",texture_name,dds_size,header_size);
    
    char *ddsdata = (char*)malloc(dds_size);//DDS_SIZE);
    assert(fread(ddsdata, 1, dds_size, dds) == dds_size);
    fclose(dds);

    glGenTextures( 1, &texture );
    glBindTexture( GL_TEXTURE_2D, texture );

    //assert(!glGetError());
    printf("glGetError = %d\n",glGetError());
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 512, 512, 0, dds_size-header_size, ddsdata+header_size);
    printf("glGetError = %d\n",glGetError());

    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );


    // Prepare and Render

    // Clear the screen before drawing
    glClear( GL_COLOR_BUFFER_BIT );

    // Bind the texture to which subsequent calls refer to
    glBindTexture( GL_TEXTURE_2D, texture );

    // Use clientside vertex pointers to render two items
    GLfloat vertexData[] = { 0, 0, 0, 0, // texture2, position2
                             1, 0, 512, 0,
                             1, 1, 512, 512,
                             0, 1, 0, 512};

    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 4*4, &vertexData[0]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 4*4, &vertexData[2]);

    glDrawArrays(GL_QUADS, 0, 4);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    SDL_GL_SwapBuffers();

    // Now we can delete the OpenGL texture and close down SDL
    glDeleteTextures( 1, &texture );
    
    // return 0;
}

#endif

// Main function
// *********************************************************************
int main(const int argc, const char** argv) 
{
    // start logs
    shrSetLogFileName ("oclDXTCompression.txt");
    shrLog(LOGBOTH, 0, "%s Starting...\n\n", argv[0]); 

    cl_context cxGPUContext;
    cl_command_queue cqCommandQueue;
    cl_program cpProgram;
    cl_kernel ckKernel;
    cl_mem cmMemObjs[3];
    size_t szGlobalWorkSize[1];
    size_t szLocalWorkSize[1];
    cl_int ciErrNum = CL_SUCCESS;

    // Get the path of the filename
    char *filename;
    if (shrGetCmdLineArgumentstr(argc, argv, "image", &filename)) {
        image_filename = filename;
    }
    // load image
    const char* image_path = shrFindFilePath(image_filename, argv[0]);
    shrCheckError(image_path != NULL, shrTRUE);
    shrLoadPPM4ub(image_path, (unsigned char **)&h_img, &width, &height);
    shrCheckError(h_img != NULL, shrTRUE);
    shrLog(LOGBOTH, 0, "Loaded '%s', %d x %d pixels\n", image_path, width, height);

    // Convert linear image to block linear. 
    uint * block_image = (uint *) malloc(width * height * 4);

    // Convert linear image to block linear. 
    for(uint by = 0; by < height/4; by++) {
        for(uint bx = 0; bx < width/4; bx++) {
            for (int i = 0; i < 16; i++) {
                const int x = i & 3;
                const int y = i / 4;
                block_image[(by * width/4 + bx) * 16 + i] = 
                    ((uint *)h_img)[(by * 4 + y) * 4 * (width/4) + bx * 4 + x];
            }
        }
    }

    // create the OpenCL context on a GPU device
    cxGPUContext = clCreateContextFromType(0, CL_DEVICE_TYPE_GPU, NULL, NULL, &ciErrNum);
    printf("Err : %d\n",ciErrNum);
    shrCheckError(ciErrNum, CL_SUCCESS);

    // get and log device
    cl_device_id device;
    if( shrCheckCmdLineFlag(argc, argv, "device") ) {
      int device_nr = 0;
      shrGetCmdLineArgumenti(argc, argv, "device", &device_nr);
      device = oclGetDev(cxGPUContext, device_nr);
    } else {
      device = oclGetMaxFlopsDev(cxGPUContext);
    }
    oclPrintDevInfo(LOGBOTH, device);

    // create a command-queue
    cqCommandQueue = clCreateCommandQueue(cxGPUContext, device, 0, &ciErrNum);
    shrCheckError(ciErrNum, CL_SUCCESS);

    // Memory Setup

    // Compute permutations.
    cl_uint permutations[1024];
    computePermutations(permutations);

    // Upload permutations.
    cmMemObjs[0] = clCreateBuffer(cxGPUContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  sizeof(cl_uint) * 1024, permutations, &ciErrNum);
    shrCheckError(ciErrNum, CL_SUCCESS);

    // Image
    cmMemObjs[1] = clCreateBuffer(cxGPUContext, CL_MEM_READ_ONLY ,
                                  sizeof(cl_uint) * width * height, NULL, &ciErrNum);
    shrCheckError(ciErrNum, CL_SUCCESS);
    
    // Result
    const uint compressedSize = (width / 4) * (height / 4) * 8;

    cmMemObjs[2] = clCreateBuffer(cxGPUContext, CL_MEM_WRITE_ONLY,
                                  compressedSize, NULL , &ciErrNum);
    shrCheckError(ciErrNum, CL_SUCCESS);
    
    unsigned int * h_result = (uint *)malloc(compressedSize);

    // Program Setup
    size_t program_length;
    const char* source_path = "DXTCompressor_kernel.cl";//shrFindFilePath("DXTCompression_kernel.cl", argv[0]);
    shrCheckError(source_path != NULL, shrTRUE);
    char *source = oclLoadProgSource(source_path, "", &program_length);
    shrCheckError(source != NULL, shrTRUE);

    // create the program
    cpProgram = clCreateProgramWithSource(cxGPUContext, 1,
        (const char **) &source, &program_length, &ciErrNum);
    shrCheckError(ciErrNum, CL_SUCCESS);

    // build the program
    ciErrNum = clBuildProgram(cpProgram, 0, NULL, "-cl-mad-enable", NULL, NULL);
    if (ciErrNum != CL_SUCCESS)
    {
        // write out standard error, Build Log and PTX, then cleanup and exit
        shrLog(LOGBOTH | ERRORMSG, ciErrNum, STDERROR);
        oclLogBuildInfo(cpProgram, oclGetFirstDev(cxGPUContext));
        oclLogPtx(cpProgram, oclGetFirstDev(cxGPUContext), "oclDXTCompression.ptx");
        shrCheckError(ciErrNum, CL_SUCCESS); 
    }

    // create the kernel
    ckKernel = clCreateKernel(cpProgram, "compress", &ciErrNum);
    shrCheckError(ciErrNum, CL_SUCCESS);

    // set the args values
    ciErrNum  = clSetKernelArg(ckKernel, 0, sizeof(cl_mem), (void *) &cmMemObjs[0]);
    ciErrNum |= clSetKernelArg(ckKernel, 1, sizeof(cl_mem), (void *) &cmMemObjs[1]);
    ciErrNum |= clSetKernelArg(ckKernel, 2, sizeof(cl_mem), (void *) &cmMemObjs[2]);
    ciErrNum |= clSetKernelArg(ckKernel, 3, sizeof(float) * 4 * 16, NULL);
    ciErrNum |= clSetKernelArg(ckKernel, 4, sizeof(float) * 4 * 16, NULL);
    ciErrNum |= clSetKernelArg(ckKernel, 5, sizeof(int) * 64, NULL);
    ciErrNum |= clSetKernelArg(ckKernel, 6, sizeof(float) * 16 * 6, NULL);
    ciErrNum |= clSetKernelArg(ckKernel, 7, sizeof(unsigned int) * 160, NULL);
    ciErrNum |= clSetKernelArg(ckKernel, 8, sizeof(int) * 16, NULL);
    shrCheckError(ciErrNum, CL_SUCCESS);

    shrLog(LOGBOTH, 0, "Running DXT Compression on %u x %u image...\n\n", width, height);

    // Upload the image
    clEnqueueWriteBuffer(cqCommandQueue, cmMemObjs[1], CL_FALSE, 0, sizeof(cl_uint) * width * height, block_image, 0,0,0);

    // set work-item dimensions
    szGlobalWorkSize[0] = width * height * (NUM_THREADS/16);
    szLocalWorkSize[0]= NUM_THREADS;
    
#ifdef GPU_PROFILING
    int numIterations = 100;
    for (int i = -1; i < numIterations; ++i) {
        if (i == 0) { // start timing only after the first warmup iteration
            clFinish(cqCommandQueue); // flush command queue
            shrDeltaT(0); // start timer
        }
#endif
        // execute kernel
        ciErrNum = clEnqueueNDRangeKernel(cqCommandQueue, ckKernel, 1, NULL,
                                          szGlobalWorkSize, szLocalWorkSize, 
                                          0, NULL, NULL);
        shrCheckError(ciErrNum, CL_SUCCESS);
#ifdef GPU_PROFILING
    }
    clFinish(cqCommandQueue);
    double dAvgTime = shrDeltaT(0) / (double)numIterations;
    shrLog(LOGBOTH | MASTER, 0, "oclDXTCompression, Throughput = %.4f, Time = %.5f, Size = %u, NumDevsUsed = %i\n", 
        (1.0e-6 * (double)(width * height)/ dAvgTime), dAvgTime, (width * height), 1); 

#endif

    // blocking read output
    ciErrNum = clEnqueueReadBuffer(cqCommandQueue, cmMemObjs[2], CL_TRUE, 0,
                                   compressedSize, h_result, 0, NULL, NULL);
    shrCheckError(ciErrNum, CL_SUCCESS);

    // Write DDS file.
    FILE* fp = NULL;
    char output_filename[1024];
    #ifdef WIN32
        strcpy_s(output_filename, 1024, image_path);
        strcpy_s(output_filename + strlen(image_path) - 3, 1024 - strlen(image_path) + 3, "dds");
        fopen_s(&fp, output_filename, "wb");
    #else
        strcpy(output_filename, image_path);
        strcpy(output_filename + strlen(image_path) - 3, "dds");
        fp = fopen(output_filename, "wb");
    #endif
    shrCheckError(fp != NULL, shrTRUE);

    DDSHeader header;
    header.fourcc = FOURCC_DDS;
    header.size = 124;
    header.flags  = (DDSD_WIDTH|DDSD_HEIGHT|DDSD_CAPS|DDSD_PIXELFORMAT|DDSD_LINEARSIZE);
    header.height = height;
    header.width = width;
    header.pitch = compressedSize;
    header.depth = 0;
    header.mipmapcount = 0;
    memset(header.reserved, 0, sizeof(header.reserved));
    header.pf.size = 32;
    header.pf.flags = DDPF_FOURCC;
    header.pf.fourcc = FOURCC_DXT1;
    header.pf.bitcount = 0;
    header.pf.rmask = 0;
    header.pf.gmask = 0;
    header.pf.bmask = 0;
    header.pf.amask = 0;
    header.caps.caps1 = DDSCAPS_TEXTURE;
    header.caps.caps2 = 0;
    header.caps.caps3 = 0;
    header.caps.caps4 = 0;
    header.notused = 0;

    fwrite(&header, sizeof(DDSHeader), 1, fp);
    fwrite(h_result, compressedSize, 1, fp);

    fclose(fp);
    
#ifdef __EMSCRIPTEN__
    
    // Print DXT Image generated
    showtexture(output_filename,sizeof(DDSHeader));
    
#endif

    // Make sure the generated image matches the reference image (regression check)
    shrLog(LOGBOTH, 0, "\nComparing against Host/C++ computation...\n");     
    const char* reference_image_path = shrFindFilePath(refimage_filename, argv[0]);
    shrCheckError(reference_image_path != NULL, shrTRUE);

    // read in the reference image from file
    #ifdef WIN32
        fopen_s(&fp, reference_image_path, "rb");
    #else
        fp = fopen(reference_image_path, "rb");
    #endif
    shrCheckError(fp != NULL, shrTRUE);
    fseek(fp, sizeof(DDSHeader), SEEK_SET);
    uint referenceSize = (width / 4) * (height / 4) * 8;
    uint * reference = (uint *)malloc(referenceSize);
    fread(reference, referenceSize, 1, fp);
    fclose(fp);

    // compare the reference image data to the sample/generated image
    float rms = 0;
    for (uint y = 0; y < height; y += 4)
    {
        for (uint x = 0; x < width; x += 4)
        {
            // binary comparison of data
            uint referenceBlockIdx = ((y/4) * (width/4) + (x/4));
            uint resultBlockIdx = ((y/4) * (width/4) + (x/4));
            int cmp = compareBlock(((BlockDXT1 *)h_result) + resultBlockIdx, ((BlockDXT1 *)reference) + referenceBlockIdx);

            // log deviations, if any
            if (cmp != 0.0f) 
            {
                compareBlock(((BlockDXT1 *)h_result) + resultBlockIdx, ((BlockDXT1 *)reference) + referenceBlockIdx);
                shrLog(LOGBOTH, 0, "Deviation at (%d, %d):\t%f rms\n", x/4, y/4, float(cmp)/16/3);
            }
            rms += cmp;
        }
    }
    rms /= width * height * 3;
    shrLog(LOGBOTH, 0, "RMS(reference, result) = %f\n\n", rms);
    shrLog(LOGBOTH, 0, "TEST %s\n\n", (rms <= ERROR_THRESHOLD) ? "PASSED" : "FAILED !!!");

    // Free OpenCL resources
    oclDeleteMemObjs(cmMemObjs, 3);
    clReleaseKernel(ckKernel);
    clReleaseProgram(cpProgram);
    clReleaseCommandQueue(cqCommandQueue);
    clReleaseContext(cxGPUContext);

    // Free host memory
    free(source);
    free(h_img);

    // finish
    shrEXIT(argc, argv);
}
