/*
 *  Photoconsistency-Visual-Odometry
 *  Multiscale Photoconsistency Visual Odometry from RGBD Images
 *  Copyright (c) 2012, Miguel Algaba Borrego
 *
 *  http://code.google.com/p/photoconsistency-visual-odometry/
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *      * Neither the name of the holder(s) nor the
 *        names of its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _CPHOTOCONSISTENCY_ODOMETRY_CERES_
#define _CPHOTOCONSISTENCY_ODOMETRY_CERES_

#define ENABLE_GAUSSIAN_BLUR 1
#define ENABLE_BOX_FILTER_BLUR 0
#define ENABLE_OPENMP_MULTITHREADING 1

#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/calib3d/calib3d.hpp"
#include "opencv2/contrib/contrib.hpp"
#include "ceres/ceres.h"

#include "opencv2/highgui/highgui.hpp" //visualize iterations

namespace PhotoconsistencyOdometryCeres
{

using ceres::AutoDiffCostFunction;
using ceres::CostFunction;
using ceres::Problem;
using ceres::Solver;
using ceres::Solve;
using namespace std;

std::vector<cv::Mat> gray0Pyr,gray1Pyr,depth0Pyr,gray1GradXPyr,gray1GradYPyr;
double cameraMatrix[3][3];
int optimizationLevel;
int numOptimizationLevels;
std::vector<int> blurFilterSize;
std::vector<float> imageGradientsScalingFactor;
std::vector<int> max_num_iterations;
std::vector<float> function_tolerance;
std::vector<float> gradient_tolerance;
std::vector<float> parameter_tolerance;
std::vector<float> initial_trust_region_radius;
std::vector<float> max_trust_region_radius;
std::vector<float> min_trust_region_radius;
std::vector<float> min_relative_decrease;
int num_linear_solver_threads;
int num_threads;
bool minimizer_progress_to_stdout;
bool visualizeIterations;
double x[6]; //Parameter vector (x y z yaw pitch roll)

void eigenPose(float x,
           float y,
           float z,
           float yaw,
           float pitch,
           float roll,
           Eigen::Matrix4f & pose)
{
    pose(0,0) = cos(yaw) * cos(pitch);
    pose(0,1) = cos(yaw) * sin(pitch) * sin(roll) - sin(yaw) * cos(roll);
    pose(0,2) = cos(yaw) * sin(pitch) * cos(roll) + sin(yaw) * sin(roll);
    pose(0,3) = x;

    pose(1,0) = sin(yaw) * cos(pitch);
    pose(1,1) = sin(yaw) * sin(pitch) * sin(roll) + cos(yaw) * cos(roll);
    pose(1,2) = sin(yaw) * sin(pitch) * cos(roll) - cos(yaw) * sin(roll);
    pose(1,3) = y;

    pose(2,0) = -sin(pitch);
    pose(2,1) = cos(pitch) * sin(roll);
    pose(2,2) = cos(pitch) * cos(roll);
    pose(2,3) = z;

    pose(3,0) = 0;
    pose(3,1) = 0;
    pose(3,2) = 0;
    pose(3,3) = 1;

}

template <class T>
void warpImage(cv::Mat & imgGray,
               cv::Mat & imgDepth,
               cv::Mat & imgGrayWarped,
               Eigen::Matrix4f & Rt,
               Eigen::Matrix3f & cameraMatrix,int level=0)
{
    float fx = cameraMatrix(0,0)/pow(2,level);
    float fy = cameraMatrix(1,1)/pow(2,level);
    float inv_fx = 1.f/fx;
    float inv_fy = 1.f/fy;
    float ox = cameraMatrix(0,2)/pow(2,level);
    float oy = cameraMatrix(1,2)/pow(2,level);

    Eigen::Vector4f point3D;
    Eigen::Vector4f transformedPoint3D;
    int transformed_r,transformed_c; // 2D coordinates of the transformed pixel(r,c) of frame 1

    imgGrayWarped = cv::Mat::zeros(imgGray.rows,imgGray.cols,imgGray.type());

    #if ENABLE_OPENMP_MULTITHREADING
    #pragma omp parallel for private(point3D,transformedPoint3D,transformed_r,transformed_c)
    #endif
    for(int r=0;r<imgGray.rows;r++)
    {
        for(int c=0;c<imgGray.cols;c++)
        {
            if(imgDepth.at<float>(r,c)>0) //If has valid depth value
            {
                //Compute the local 3D coordinates of pixel(r,c) of frame 1
                point3D(2) = imgDepth.at<float>(r,c); //z
                point3D(0) = (c-ox) * point3D(2) * inv_fx;	   //x
                point3D(1) = (r-oy) * point3D(2) * inv_fy;	   //y
                point3D(3) = 1.0;			   //homogeneous coordinate

                //Transform the 3D point using the transformation matrix Rt
                transformedPoint3D = Rt * point3D;

                //Project the 3D point to the 2D plane
                transformed_c = ((transformedPoint3D(0) * fx) / transformedPoint3D(2)) + ox; //transformed x (2D)
                transformed_r = ((transformedPoint3D(1) * fy) / transformedPoint3D(2)) + oy; //transformed y (2D)

                //Asign the intensity value to the warped image and compute the difference between the transformed
                //pixel of frame 1 and the corresponding pixel of frame 2. Compute the error function
                if(transformed_r>=0 && transformed_r < imgGray.rows &
                   transformed_c>=0 && transformed_c < imgGray.cols)
                {
                    imgGrayWarped.at<T>(transformed_r,transformed_c)=imgGray.at<T>(r,c);
                }
            }
        }
    }
}

void LinearInitAxis(double x, int size,
                    int *x1, int *x2,
                    double *dx)
{
  const int ix = static_cast<int>(x);
  if (ix < 0) {
    *x1 = 0;
    *x2 = 0;
    *dx = 1.0;
  } else if (ix > size - 2) {
    *x1 = size - 1;
    *x2 = size - 1;
    *dx = 1.0;
  } else {
    *x1 = ix;
    *x2 = ix + 1;
    *dx = *x2 - x;
  }
}

/// Linear interpolation.
template<typename T>
void SampleLinear(const cv::Mat & intensityImage,
                  const cv::Mat & intensityGradientX,
                  const cv::Mat & intensityGradientY,
                  double y, double x, T* sample) {

  int x1, y1, x2, y2;
  double dx, dy;

  // Take the upper left corner as integer pixel positions.
  x -= 0.5;
  y -= 0.5;

  LinearInitAxis(y, intensityImage.rows, &y1, &y2, &dy);
  LinearInitAxis(x, intensityImage.cols,  &x1, &x2, &dx);

  //Sample intensity
  const T im11 = T(intensityImage.at<float>(y1, x1));
  const T im12 = T(intensityImage.at<float>(y1, x2));
  const T im21 = T(intensityImage.at<float>(y2, x1));
  const T im22 = T(intensityImage.at<float>(y2, x2));

  sample[0] =(     dy  * ( dx * im11 + (1.0 - dx) * im12 ) +
           (1 - dy) * ( dx * im21 + (1.0 - dx) * im22 ));

  //Sample gradient x
  const T gradx11 = T(intensityGradientX.at<float>(y1, x1));
  const T gradx12 = T(intensityGradientX.at<float>(y1, x2));
  const T gradx21 = T(intensityGradientX.at<float>(y2, x1));
  const T gradx22 = T(intensityGradientX.at<float>(y2, x2));

  sample[1] =(     dy  * ( dx * gradx11 + (1.0 - dx) * gradx12 ) +
           (1 - dy) * ( dx * gradx21 + (1.0 - dx) * gradx22 ));

  //Sample gradient y
  const T grady11 = T(intensityGradientY.at<float>(y1, x1));
  const T grady12 = T(intensityGradientY.at<float>(y1, x2));
  const T grady21 = T(intensityGradientY.at<float>(y2, x1));
  const T grady22 = T(intensityGradientY.at<float>(y2, x2));

  sample[2] =(     dy  * ( dx * grady11 + (1.0 - dx) * grady12 ) +
           (1 - dy) * ( dx * grady21 + (1.0 - dx) * grady22 ));
}

// Sample the image at position (x, y) but use the gradient to
// propagate derivatives from x and y. This is needed to integrate the numeric
// image gradients with Ceres's autodiff framework.
template<typename T>
T SampleWithDerivative(const cv::Mat & intensityImage,
                              const cv::Mat & intensityGradientX,
                              const cv::Mat & intensityGradientY,
                              const T & x,
                              const T & y)
{
  double scalar_x = ceres::JetOps<T>::GetScalar(x);
  double scalar_y = ceres::JetOps<T>::GetScalar(y);

  double sample[3];
  // Sample intensity image and gradients
  SampleLinear(intensityImage,intensityGradientX,intensityGradientY, scalar_y, scalar_x, sample);
  T xy[2] = { x, y };
  return ceres::Chain<double, 2, T>::Rule(sample[0], sample + 1, xy);
}

class CPhotoconsistencyOdometryCeres
{

private:

    class ResidualRGBDPhotoconsistency {

     public:

      template <typename T> bool operator()(const T* const stateVector,
                                            T* residuals) const {

        //Set camera parameters depending on the optimization level
        T fx = T(cameraMatrix[0][0])/pow(2,T(optimizationLevel));
        T fy = T(cameraMatrix[1][1])/pow(2,T(optimizationLevel));
        T inv_fx = T(1)/fx;
        T inv_fy = T(1)/fy;
        T ox = T(cameraMatrix[0][2])/pow(2,T(optimizationLevel));
        T oy = T(cameraMatrix[1][2])/pow(2,T(optimizationLevel));

        //Compute the rigid transformation matrix from the parameters
        T x = stateVector[0];
        T y = stateVector[1];
        T z = stateVector[2];
        T yaw = stateVector[3];
        T pitch = stateVector[4];
        T roll = stateVector[5];
        T Rt[4][4];
        T sin_yaw = sin(yaw);
        T cos_yaw = cos(yaw);
        T sin_pitch = sin(pitch);
        T cos_pitch = cos(pitch);
        T sin_roll = sin(roll);
        T cos_roll = cos(roll);
        Rt[0][0] = cos_yaw * cos_pitch;
        Rt[0][1] = cos_yaw * sin_pitch * sin_roll - sin_yaw * cos_roll;
        Rt[0][2] = cos_yaw * sin_pitch * cos_roll + sin_yaw * sin_roll;
        Rt[0][3] = x;
        Rt[1][0] = sin_yaw * cos_pitch;
        Rt[1][1] = sin_yaw * sin_pitch * sin_roll + cos_yaw * cos_roll;
        Rt[1][2] = sin_yaw * sin_pitch * cos_roll - cos_yaw * sin_roll;
        Rt[1][3] = y;
        Rt[2][0] = -sin_pitch;
        Rt[2][1] = cos_pitch * sin_roll;
        Rt[2][2] = cos_pitch * cos_roll;
        Rt[2][3] = z;
        Rt[3][0] = T(0);
        Rt[3][1] = T(0);
        Rt[3][2] = T(0);
        Rt[3][3] = T(1);

        //Initialize the error function (residuals) with an initial value
        #if ENABLE_OPENMP_MULTITHREADING
        #pragma omp parallel for
        #endif
        for(int r=0;r<gray0Pyr[optimizationLevel].rows;r++)
        {
            for(int c=0;c<gray0Pyr[optimizationLevel].cols;c++)
            {
                residuals[gray0Pyr[optimizationLevel].cols*r+c]=T(0);
            }
        }

        T residualScalingFactor = T(1);

        #if ENABLE_OPENMP_MULTITHREADING
        #pragma omp parallel for
        #endif
        for(int r=0;r<gray0Pyr[optimizationLevel].rows;r++)
        {

            T point3D[4];
            T transformedPoint3D[4];
            T transformed_r,transformed_c; // 2D coordinates of the transformed pixel(r,c) of frame 1
            T pixel1; //Intensity value of the pixel(r,c) of the warped frame 1
            T pixel2; //Intensity value of the pixel(r,c) of frame 2

            for(int c=0;c<gray0Pyr[optimizationLevel].cols;c++)
            {
                if(depth0Pyr[optimizationLevel].at<float>(r,c)>0) //If has valid depth value
                {
                    //Compute the local 3D coordinates of pixel(r,c) of frame 1
                    point3D[2] = T(depth0Pyr[optimizationLevel].at<float>(r,c)); //z
                    point3D[0] = (T(c)-ox) * point3D[2] * inv_fx;	   //x
                    point3D[1] = (T(r)-oy) * point3D[2] * inv_fy;	   //y
                    point3D[3] = T(1.0);			   //homogeneous coordinate

                    //Transform the 3D point using the transformation matrix Rt
                    transformedPoint3D[0] = Rt[0][0]*point3D[0]+Rt[0][1]*point3D[1]+Rt[0][2]*point3D[2]+Rt[0][3]*point3D[3];
                    transformedPoint3D[1] = Rt[1][0]*point3D[0]+Rt[1][1]*point3D[1]+Rt[1][2]*point3D[2]+Rt[1][3]*point3D[3];
                    transformedPoint3D[2] = Rt[2][0]*point3D[0]+Rt[2][1]*point3D[1]+Rt[2][2]*point3D[2]+Rt[2][3]*point3D[3];
                    transformedPoint3D[3] = Rt[3][0]*point3D[0]+Rt[3][1]*point3D[1]+Rt[3][2]*point3D[2]+Rt[3][3]*point3D[3];

                    //Project the 3D point to the 2D plane
                    transformed_c = ((transformedPoint3D[0] * fx) / transformedPoint3D[2]) + ox; //transformed x (2D)
                    transformed_r = ((transformedPoint3D[1] * fy) / transformedPoint3D[2]) + oy; //transformed y (2D)

                    //Asign the intensity value to the warped image and compute the difference between the transformed
                    //pixel of frame 1 and the corresponding pixel of frame 2. Compute the error function
                    if(transformed_r>=T(0) && transformed_r < T(gray0Pyr[optimizationLevel].rows) &
                       transformed_c>=T(0) && transformed_c < T(gray0Pyr[optimizationLevel].cols))
                    {
                        //Compute the proyected coordinates of the transformed 3D point
                        int transformed_r_scalar = static_cast<int>(ceres::JetOps<T>::GetScalar(transformed_r));
                        int transformed_c_scalar = static_cast<int>(ceres::JetOps<T>::GetScalar(transformed_c));

                        //Compute the pixel residual
                        pixel1 = T(gray0Pyr[optimizationLevel].at<float>(r,c));
                        pixel2 = SampleWithDerivative(gray1Pyr[optimizationLevel],
                                                      gray1GradXPyr[optimizationLevel],
                                                      gray1GradYPyr[optimizationLevel],transformed_c,transformed_r);
                        residuals[gray0Pyr[optimizationLevel].cols*transformed_r_scalar+transformed_c_scalar] = residualScalingFactor * (pixel1 - pixel2);

                    }
                }
            }
        }

        return true;
      }
    };

    class VisualizationCallback: public ceres::IterationCallback {
    public:
        virtual ceres::CallbackReturnType operator()(const ceres::IterationSummary& summary)
        {
            Eigen::Matrix3f cameraMatrix_eigen;
            for(int i=0;i<3;i++)
            {
                for(int j=0;j<3;j++)
                {
                    cameraMatrix_eigen(i,j)=cameraMatrix[i][j];
                }
            }

            /*Eigen::Matrix4f Rt;
            eigenPose(x[0],x[1],x[2],x[3],x[4],x[5],Rt);
            std::cout<<"Rt eigen:"<<std::endl<<Rt<<std::endl;
            cv::Mat warpedImage;
            warpImage<float>(gray0Pyr[optimizationLevel],depth0Pyr[optimizationLevel],warpedImage,Rt,cameraMatrix_eigen,optimizationLevel);
            cv::Mat imgDiff;
            cv::absdiff(gray1Pyr[optimizationLevel],warpedImage,imgDiff);
            cv::imshow("callback: imgDiff",imgDiff);
            cv::waitKey(5);*/

            Eigen::Matrix4f Rt;
            eigenPose(x[0],x[1],x[2],x[3],x[4],x[5],Rt);
            std::cout<<"Rt eigen:"<<std::endl<<Rt<<std::endl;
            cv::Mat warpedImage;
            warpImage<float>(gray0Pyr[0],depth0Pyr[0],warpedImage,Rt,cameraMatrix_eigen);
            cv::Mat imgDiff;
            cv::absdiff(gray1Pyr[0],warpedImage,imgDiff);
            cv::imshow("callback: imgDiff",imgDiff);
            cv::waitKey(5);

            return ceres::SOLVER_CONTINUE;
        }
    };

    void buildPyramid(cv::Mat & img,std::vector<cv::Mat>& pyramid,int levels,bool applyBlur)
    {
        //Create space for all the images
        pyramid.resize(levels);

        double factor = 1;
        for(int level=0;level<levels;level++)
        {
            //Create an auxiliar image of factor times the size of the original image
            cv::Mat imgAux;
            if(level!=0)
            {
                cv::resize(img,imgAux,cv::Size(0,0),factor,factor);
            }
            else
            {
                imgAux = img;
            }

            //Blur the resized image with different filter size depending on the current pyramid level
            if(applyBlur)
            {
                #if ENABLE_GAUSSIAN_BLUR
                if(blurFilterSize[level]>0)
                {
                    cv::GaussianBlur(imgAux,imgAux,cv::Size(blurFilterSize[level],blurFilterSize[level]),3);
                    cv::GaussianBlur(imgAux,imgAux,cv::Size(blurFilterSize[level],blurFilterSize[level]),3);
                }
                #elif ENABLE_BOX_FILTER_BLUR
                if(blurFilterSize[level]>0)
                {
                    cv::blur(imgAux,imgAux,cv::Size(blurFilterSize[level],blurFilterSize[level]));
                    cv::blur(imgAux,imgAux,cv::Size(blurFilterSize[level],blurFilterSize[level]));
                }
                #endif
            }

            //Assign the resized image to the current level of the pyramid
            pyramid[level]=imgAux;

            factor = factor/2;
        }
    }

    void buildDerivativesPyramids(std::vector<cv::Mat>& imagePyramid,std::vector<cv::Mat>& derXPyramid,std::vector<cv::Mat>& derYPyramid)
    {
        //Compute image gradients
        int scale = 1;
        int delta = 0;
        int ddepth = CV_32FC1;

        //Create space for all the derivatives images
        derXPyramid.resize(imagePyramid.size());
        derYPyramid.resize(imagePyramid.size());

        for(int level=0;level<imagePyramid.size();level++)
        {
            // Compute the gradient in x
            cv::Mat imgGray1_grad_x;
            cv::Scharr( imagePyramid[level], derXPyramid[level], ddepth, 1, 0, imageGradientsScalingFactor[level], delta, cv::BORDER_DEFAULT );

            // Compute the gradient in y
            cv::Mat imgGray1_grad_y;
            cv::Scharr( imagePyramid[level], derYPyramid[level], ddepth, 0, 1, imageGradientsScalingFactor[level], delta, cv::BORDER_DEFAULT );
        }
    }

public:

    CPhotoconsistencyOdometryCeres(){};

    ~CPhotoconsistencyOdometryCeres(){};

    void setCameraMatrix(Eigen::Matrix3f & camMat)
    {
        for(int i=0;i<3;i++)
        {
            for(int j=0;j<3;j++)
            {
                cameraMatrix[i][j]=camMat(i,j);
            }
        }
    }

    void setSourceFrame(cv::Mat & imgGray,cv::Mat & imgDepth)
    {
        //Create a float auxialiary image from the imput image
        cv::Mat imgGrayFloat;
        imgGray.convertTo(imgGrayFloat, CV_32FC1, 1./255 );

        //Compute image pyramids for the grayscale and depth images
        buildPyramid(imgGrayFloat,gray0Pyr,numOptimizationLevels,true);
        buildPyramid(imgDepth,depth0Pyr,numOptimizationLevels,false);
    }

    void setTargetFrame(cv::Mat & imgGray)
    {
        //Create a float auxialiary image from the imput image
        cv::Mat imgGrayFloat;
        imgGray.convertTo(imgGrayFloat, CV_32FC1, 1./255 );

        //Compute image pyramids for the grayscale and depth images
        buildPyramid(imgGrayFloat,gray1Pyr,numOptimizationLevels,true);

        //Compute image pyramids for the gradients images
        buildDerivativesPyramids(gray1Pyr,gray1GradXPyr,gray1GradYPyr);
    }

    void setInitialStateVector(const std::vector<double> & initialStateVector)
    {
        x[0] = initialStateVector[0];
        x[1] = initialStateVector[1];
        x[2] = initialStateVector[2];
        x[3] = initialStateVector[3];
        x[4] = initialStateVector[4];
        x[5] = initialStateVector[5];
    }

    void optimize()
    {
        for(optimizationLevel = numOptimizationLevels-1;optimizationLevel>=0;optimizationLevel--)
        {
            // Build the problem.
            Problem problem;

            // Set up the only cost function (also known as residual). This uses
            // auto-differentiation to obtain the derivative (jacobian).
            problem.AddResidualBlock(
            new AutoDiffCostFunction<ResidualRGBDPhotoconsistency,ceres::DYNAMIC,6>(
              new ResidualRGBDPhotoconsistency,
              gray0Pyr[optimizationLevel].cols*gray0Pyr[optimizationLevel].rows /*dynamic size*/),
            NULL,
            x);

            // Run the solver!
            Solver::Options options;
            options.max_num_iterations = max_num_iterations[optimizationLevel];
            options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;//ceres::DENSE_QR;
            options.minimizer_progress_to_stdout = minimizer_progress_to_stdout;
            options.function_tolerance = function_tolerance[optimizationLevel];
            options.gradient_tolerance = gradient_tolerance[optimizationLevel];
            options.parameter_tolerance = parameter_tolerance[optimizationLevel];
            options.initial_trust_region_radius = initial_trust_region_radius[optimizationLevel];
            options.max_trust_region_radius = max_trust_region_radius[optimizationLevel];
            options.min_trust_region_radius = min_trust_region_radius[optimizationLevel];
            options.min_relative_decrease = min_relative_decrease[optimizationLevel];
            options.num_linear_solver_threads = num_linear_solver_threads;
            options.num_threads = num_threads;
            options.max_num_consecutive_invalid_steps = 0;
            VisualizationCallback callback;
            if(visualizeIterations)
            {
                options.update_state_every_iteration = true;
                options.callbacks.push_back(&callback);
            }
            else
            {
                options.update_state_every_iteration = false;
            }

            Solver::Summary summary;
            Solve(options, &problem, &summary);
            std::cout << summary.BriefReport() << "\n";
        }

        //After all the optimization process the optimization level is 0
        optimizationLevel = 0;

    }

    void getOptimalStateVector(std::vector<double> & optimalStateVector)
    {
        optimalStateVector[0] = x[0];
        optimalStateVector[1] = x[1];
        optimalStateVector[2] = x[2];
        optimalStateVector[3] = x[3];
        optimalStateVector[4] = x[4];
        optimalStateVector[5] = x[5];
    }

    void getOptimalRigidTransformationMatrix(Eigen::Matrix4f & optimal_Rt)
    {
        eigenPose(x[0],x[1],x[2],
                  x[3],x[4],x[5],optimal_Rt);
    }

    void readConfigurationFile(std::string fileName)
    {
        cv::FileStorage fs(fileName, cv::FileStorage::READ);

        //Read the number of optimization levels
        fs["numOptimizationLevels"] >> numOptimizationLevels;

        #if ENABLE_GAUSSIAN_BLUR || ENABLE_BOX_FILTER_BLUR
        //Read the blur filter size at every pyramid level
        fs["blurFilterSize (at each level)"] >> blurFilterSize;
        #endif

        //Read the scaling factor for each gradient image at each level
        fs["imageGradientsScalingFactor (at each level)"] >> imageGradientsScalingFactor;

        //Read the number of Levenberg-Marquardt iterations at each optimization level
        fs["max_num_iterations (at each level)"] >> max_num_iterations;

        //Read optimizer function tolerance at each level
        fs["function_tolerance (at each level)"] >> function_tolerance;

        //Read optimizer gradient tolerance at each level
        fs["gradient_tolerance (at each level)"] >> gradient_tolerance;

        //Read optimizer parameter tolerance at each level
        fs["parameter_tolerance (at each level)"] >> parameter_tolerance;

        //Read optimizer initial trust region at each level
        fs["initial_trust_region_radius (at each level)"] >> initial_trust_region_radius;

        //Read optimizer max trust region radius at each level
        fs["max_trust_region_radius (at each level)"] >> max_trust_region_radius;

        //Read optimizer min trust region radius at each level
        fs["min_trust_region_radius (at each level)"] >> min_trust_region_radius;

        //Read optimizer min LM relative decrease at each level
        fs["min_relative_decrease (at each level)"] >> min_relative_decrease;

        //Read the number of threads for the linear solver
        fs["num_linear_solver_threads"] >> num_linear_solver_threads;

        //Read the number of threads for the jacobian computation
        fs["num_threads"] >> num_threads;

        //Read the boolean value to determine if print the minimization progress or not
        fs["minimizer_progress_to_stdout"] >> minimizer_progress_to_stdout;

        //Read the boolean value to determine if visualize the progress images or not
        fs["visualizeIterations"] >> visualizeIterations;
    }

};

} //end namespace

#endif
