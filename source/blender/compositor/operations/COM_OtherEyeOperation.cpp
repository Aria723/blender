/*
* Copyright 2011, Blender Foundation.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software Foundation,
* Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
* Contributor:
*		Tod Baudais
*/

#include "COM_OtherEyeOperation.h"
#include "MEM_guardedalloc.h"
#include "BKE_object.h"
#include "DNA_object_types.h"
#include "DNA_camera_types.h"

#include <iostream>
#include <limits>

#define INDEX_COL(x,y) ((y * getWidth() + x) * COM_NUM_CHANNELS_COLOR)
#define INDEX_VAL(x,y) ((y * getWidth() + x) * COM_NUM_CHANNELS_VALUE)

extern "C" {
    void camera_stereo3d_model_matrix(Object *camera, const bool is_left, float r_modelmat[4][4]);
}

OtherEyeOperation::OtherEyeOperation() : NodeOperation()
{
	addInputSocket(COM_DT_COLOR);
	addInputSocket(COM_DT_VALUE); // ZBUF
    addOutputSocket(COM_DT_COLOR); // Other
	m_inputImageProgram = NULL;
	m_inputDepthProgram = NULL;
	m_cachedInstance = NULL;
	setComplex(true);
}
void OtherEyeOperation::initExecution()
{
	initMutex();
	m_inputImageProgram = getInputSocketReader(0);
	m_inputDepthProgram = getInputSocketReader(1);
	m_cachedInstance = NULL;
}

void OtherEyeOperation::deinitExecution()
{
	deinitMutex();
	if (m_cachedInstance) {
		MEM_freeN(m_cachedInstance);
		m_cachedInstance = NULL;
	}
}

void *OtherEyeOperation::initializeTileData(rcti *rect)
{
	if (m_cachedInstance) {
		return m_cachedInstance;
	}
	
	lockMutex();
	if (m_cachedInstance == NULL) {
		MemoryBuffer *color = (MemoryBuffer *)m_inputImageProgram->initializeTileData(rect);
        MemoryBuffer *depth = (MemoryBuffer *)m_inputDepthProgram->initializeTileData(rect);

        float *data = (float *)MEM_callocN(MEM_allocN_len(color->getBuffer()), "Other eye data buffer");

        // Camera matrices
        float left_to_world[4][4];
        float world_to_right[4][4];
        float width = getWidth();
        float height = getHeight();
        float A = 1.0F;
        float B = 1.0F;

        Object *camera = (Object*) m_camera;
        if (camera) {
			CameraParams params;

			// Still need to set up left_to_world and world_to_right
			Compute_eye_matrices(camera, &params, (float)getWidth(), (float)getHeight(), left_to_world, world_to_right);
			// c == 0: Left eye to world
			// c == 1: World to right eye
    //        for (int c = 0; c < 2; ++c) {
    //            CameraParams params;
    //        
    //            float viewinv[4][4];
    //            float viewmat[4][4];
    //            camera_stereo3d_model_matrix(camera, c == 0, viewmat);
    //            
    //            // Objmat
    //            //normalize_m4(viewmat);
    //            invert_m4_m4(viewinv, viewmat);

    //            // Window matrix, clipping and ortho
    //            BKE_camera_params_init(&params);
    //            BKE_camera_params_from_object(&params, camera);
				//compute_auto_viewplane(&params, getWidth(), getHeight());
    //            //BKE_camera_params_compute_matrix(&params);
				//ComputePerspectiveMatrix(&params);
    //            
    //            // Framebuffer matrix
    //            float fbmat[4][4];
    //            zero_m4(fbmat);
    //            fbmat[0][0] = width*0.5F;   fbmat[3][0] = 0.0;
    //            fbmat[1][1] = height*0.5F;  fbmat[3][1] = 0.0;
    //            fbmat[2][2] = 1.0F;         fbmat[3][2] = 0.0F;
    //            fbmat[3][3] = 1.0F;

    //            mul_m4_m4m4(viewinv, params.winmat, viewinv);
    //            mul_m4_m4m4(viewinv, fbmat, viewinv);
    //            
				//if (c == 0) {
    //                invert_m4_m4(left_to_world, viewinv);
    //                A = params.winmat[2][2];
    //                B = params.winmat[3][2];
    //                
    //                // mat[2][2] = A = -(farClip + nearClip) / (farClip - nearClip);
    //                // mat[3][2] = B = (-2.0f * nearClip * farClip) / (farClip - nearClip);
    //            } else {
    //                copy_m4_m4(world_to_right, viewinv);
    //            }
    //            
    //        }

        } else {
            unit_m4(left_to_world);
            unit_m4(world_to_right);
        }

        generateReprojection(color, depth, data, left_to_world, world_to_right, A, B);

		m_cachedInstance = data;
	}
	unlockMutex();
	return m_cachedInstance;
}

void OtherEyeOperation::Compute_eye_matrices(Object *camera, CameraParams *params, int width, int height, float left_to_world[4][4], float world_to_right[4][4])
{
	/* Full viewport to camera border in the viewport. */
	float fulltoborder[4][4];
	float bordertofull[4][4];
	transform_from_viewplane(0.0f, 1.0f, 0.0f, 1.0f, fulltoborder);
	invert_m4_m4(bordertofull, fulltoborder);

	/* ndc to raster */
	float ndctoraster[4][4];
	mul_m4_m4m4(ndctoraster, (float(*)[4])transform_scale(getWidth(), getHeight(), 1.0f), bordertofull);

	/* raster to screen */
	float screentondc[4][4];
	float screentoraster[4][4];
	float rastertoscreen[4][4];
	float viewplane[4][4];
	transform_from_viewplane(-1.0f, 1.0f, -1.0f / ((float)getWidth() / (float)getHeight()), 
		1.0f / ((float)getWidth() / (float)getHeight()), viewplane);
	mul_m4_m4m4(screentondc, fulltoborder, viewplane);
	mul_m4_m4m4(screentoraster, ndctoraster, screentondc);
	invert_m4_m4(rastertoscreen, screentoraster);

	/* screen to camera */
	float cameratoscreen[4][4];
	float screentocamera[4][4];
	ComputePerspectiveMatrix(params);
	copy_m4_m4(cameratoscreen, params->winmat);
	invert_m4_m4(screentocamera, cameratoscreen);

	/* camera to raster */
	float rastertocamera[4][4];
	float cameratoraster[4][4];
	mul_m4_m4m4(rastertocamera, screentocamera, rastertoscreen);
	mul_m4_m4m4(cameratoraster, screentoraster, cameratoscreen);

	/* world to raster */
	float cameratoworld[4][4];
	float screentoworld[4][4];
	float rastertoworld[4][4];
	float ndctoworld[4][4];
	copy_m4_m4(cameratoworld, camera->obmat);
	mul_m4_m4m4(screentoworld, cameratoworld, screentocamera);
	mul_m4_m4m4(rastertoworld, cameratoworld, rastertocamera);
	mul_m4_m4m4(ndctoworld, rastertoworld, ndctoraster);

	/* world to raster */
	float worldtocamera[4][4];
	float worldtoscreen[4][4];
	float worldtondc[4][4];
	float worldtoraster[4][4];
	invert_m4_m4(worldtocamera, camera->obmat);
	mul_m4_m4m4(worldtoscreen, cameratoscreen, worldtocamera);
	mul_m4_m4m4(worldtondc, screentondc, worldtoscreen);
	mul_m4_m4m4(worldtoraster, ndctoraster, worldtondc);

	/* copying matrices to get left_to_world and right_to_world */

}

void OtherEyeOperation::transform_from_viewplane(float left, float right, float bottom, float top, float transformation[4][4])
{
	// scale matrix
	float scale[4][4];
	zero_m4(scale);
	scale[0][0] = 1.0f / (right - left);
	scale[1][1] = 1.0f / (top - bottom);
	scale[2][2] = 1.0f;
	scale[3][3] = 1.0f;

	// translate matrix
	float translate[4][4];
	zero_m4(translate);
	translate_m4(translate, -left, -bottom, 0.0f);

	mul_m4_m4m4(transformation, scale, translate);
}

float** OtherEyeOperation::transform_scale(float x, float y, float z)
{
	float** scale = 0;

	zero_m4((float(*)[4])scale);
	scale[0][0] = x;
	scale[1][1] = y;
	scale[2][2] = z;
	scale[3][3] = 1.0f;

	return scale;
}

void OtherEyeOperation::compute_auto_viewplane(CameraParams *params, int width, int height)
{
	float aspect = (float)width / (float)height;
	if (width >= height) {
		params->viewplane.xmin = -aspect;
		params->viewplane.xmax =  aspect;
		params->viewplane.ymin = -1.0f;
		params->viewplane.ymax =  1.0f;
	}
	else {
		params->viewplane.xmin = -1.0f;
		params->viewplane.xmax =  1.0f;
		params->viewplane.ymin = -1.0f / aspect;
		params->viewplane.ymax =  1.0f / aspect;
	}
}

void OtherEyeOperation::ComputePerspectiveMatrix(CameraParams *params)
{
	params->winmat[0][1] = params->winmat[0][3] = params->winmat[1][0] =
	params->winmat[1][3] = params->winmat[2][0] = params->winmat[2][1] =
	params->winmat[3][0] = params->winmat[3][1] = params->winmat[3][3] = 0.0;
	params->winmat[0][0] = 1.09375; params->winmat[1][1] = 1.94444454;
	params->winmat[2][2] = 1.00100100; params->winmat[2][3] = -0.100100100;
	params->winmat[3][2] = 1.0;
	params->winmat[0][2] = params->winmat[1][2] = 0.5;
}

void OtherEyeOperation::drawTriangle(float *data, float *depth_buffer,
                                     float vt1[2], float c1[4], float d1,
                                     float vt2[2], float c2[4], float d2,
                                     float vt3[2], float c3[4], float d3)
{
    /* spanning vectors of edge (v1,v2) and (v1,v3) */
    float vs1[2] = {vt2[0] - vt1[0], vt2[1] - vt1[1]};
    float vs2[2] = {vt3[0] - vt1[0], vt3[1] - vt1[1]};
    
    float den = 1.0F / cross_v2v2(vs1, vs2);
    mul_v2_fl(vs1,den);
    mul_v2_fl(vs2,den);

    float minX = std::min(std::min(vt1[0], vt2[0]), vt3[0]);
    float maxX = std::max(std::max(vt1[0], vt2[0]), vt3[0]);
    float minY = std::min(std::min(vt1[1], vt2[1]), vt3[1]);
    float maxY = std::max(std::max(vt1[1], vt2[1]), vt3[1]);
    
    if (minX < 0)   minX = 0;
    if (minY < 0)   minY = 0;
    if (maxX >= getWidth())  maxX = getWidth()-1;
    if (maxY >= getHeight()) maxY = getHeight()-1;
    
    for (int x = minX; x <= maxX; x++) {
        for (int y = minY; y <= maxY; y++) {
            float q[2] = {x - vt1[0], y - vt1[1]};

            float s = cross_v2v2(q, vs2);
            float t = cross_v2v2(vs1, q);

            if ( (s >= 0) && (t >= 0) && (s + t <= 1)) {
                float u = 1 - (s+t);

                float di = s*d2 + t*d3 + u*d1; // Interpolated depth
                
                int index_val = INDEX_VAL(x, y);
                float *depth_pixel = depth_buffer + index_val;
                
                if (di > *depth_pixel) {
                    float ci[4]; // Interpolated color
                    ci[0] = s*c2[0] + t*c3[0] + u*c1[0];
                    ci[1] = s*c2[1] + t*c3[1] + u*c1[1];
                    ci[2] = s*c2[2] + t*c3[2] + u*c1[2];
                    ci[3] = s*c2[3] + t*c3[3] + u*c1[3];
                    
                    int index_col = INDEX_COL(x, y);
                    float *data_pixel = data + index_col;

                    data_pixel[0] = ci[0];
                    data_pixel[1] = ci[1];
                    data_pixel[2] = ci[2];
                    data_pixel[3] = ci[3];
                }
            }
        }
    }
}

void OtherEyeOperation::reprojectLeftToRight(float r[3], float l[3], float left_to_world[4][4], float world_to_right[4][4], float A, float B)
{

    // Correct z
    auto world_depth = l[2];
    
    // i.e. near_clip = -1.0, far clip = 1.0
    float normalized_depth = (-A * world_depth + B) / world_depth;
    
    // Build 4 component vector
    float l4[4] = {l[0], l[1], normalized_depth, 1.0F};
    float r4[4];

    // Left camera to world transformation
    float w[4];
    mul_v4_m4v4(w, left_to_world, l4);
//    w[0] /= w[3];
//    w[1] /= w[3];
//    w[2] /= w[3];
//    w[3] = 1.0F;

    // w should be in world space now

    // World to right eye
    mul_v4_m4v4(r4, world_to_right, w);
    
    // Round to fix any round off error
    r[0] = roundf(r4[0]/r4[3]);
    r[1] = roundf(r4[1]/r4[3]);
    r[2] = roundf(r4[2]/r4[3]);
}

void OtherEyeOperation::generateReprojection(MemoryBuffer *color, MemoryBuffer *depth, float *data, float left_to_world[4][4], float world_to_right[4][4], float A, float B)
{
	float *depth_buffer = (float *) MEM_callocN(MEM_allocN_len(depth->getBuffer()), "Other eye depth buffer");

    int width = getWidth();
    int height = getHeight();
    
    for (int y = 0; y < height-1; ++y) {
        for (int x = 0; x < width-1; ++x) {
        
            float *color_pixel_00 = color->getBuffer() + INDEX_COL(x, y);
            float *color_pixel_10 = color->getBuffer() + INDEX_COL(x+1, y);
            float *color_pixel_01 = color->getBuffer() + INDEX_COL(x, y+1);
            float *color_pixel_11 = color->getBuffer() + INDEX_COL(x+1, y+1);

            float *depth_pixel_00 = depth->getBuffer() + INDEX_VAL(x, y);
            float *depth_pixel_10 = depth->getBuffer() + INDEX_VAL(x+1, y);
            float *depth_pixel_01 = depth->getBuffer() + INDEX_VAL(x, y+1);
            float *depth_pixel_11 = depth->getBuffer() + INDEX_VAL(x+1, y+1);

            float l_00[3] = {x, y, *depth_pixel_00};
            float r_00[3];

            float l_10[3] = {x+1, y, *depth_pixel_10};
            float r_10[3];

            float l_01[3] = {x, y+1, *depth_pixel_01};
            float r_01[3];

            float l_11[3] = {x+1, y+1, *depth_pixel_11};
            float r_11[3];
        
            reprojectLeftToRight(r_00, l_00, left_to_world, world_to_right, A, B);
            reprojectLeftToRight(r_10, l_10, left_to_world, world_to_right, A, B);
            reprojectLeftToRight(r_01, l_01, left_to_world, world_to_right, A, B);
            reprojectLeftToRight(r_11, l_11, left_to_world, world_to_right, A, B);

            drawTriangle(data, depth_buffer,
                         r_00, color_pixel_00, r_00[2],
                         r_11, color_pixel_11, r_11[2],
                         r_10, color_pixel_10, r_10[2]);
            drawTriangle(data, depth_buffer,
                         r_00, color_pixel_00, r_00[2],
                         r_11, color_pixel_11, r_11[2],
                         r_01, color_pixel_01, r_01[2]);
        }
    }

    MEM_freeN(depth_buffer);
}


void OtherEyeOperation::executePixel(float output[4], int x, int y, void *data)
{
	float *buffer = (float *)data;
	copy_v4_v4(output, &buffer[INDEX_COL(x, y)]);
}

bool OtherEyeOperation::determineDependingAreaOfInterest(rcti *input, ReadBufferOperation *readOperation, rcti *output)
{
	if (m_cachedInstance == NULL) {
		rcti newInput;
		newInput.xmax = getWidth();
		newInput.xmin = 0;
		newInput.ymax = getHeight();
		newInput.ymin = 0;
		return NodeOperation::determineDependingAreaOfInterest(&newInput, readOperation, output);
	}
	else {
		return false;
	}
}




