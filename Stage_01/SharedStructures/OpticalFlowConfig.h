// OpticalFlowConfig.h

//#pragma once
#ifndef OPTICAL_FLOW_CONFIG_H
#define OPTICAL_FLOW_CONFIG_H

struct OpticalFlowConfig {
        
    bool validate() const {
        return windowSize > 0 && maxLevel >= 0;
    }
    int windowSize=15; // LK integration window size
    float threshold; // matrix inversion threshold,  Add threshold member
    int maxLevel=2; // number of pyramid levels
    int maxIterations; // max iterations for Lucas-Kanade
    int minEigenValue; // minimum eigenvalue for corner detection
    int maxCorners; // max corners to detect
    int blockSize; // block size for corner detection
    int useHarrisDetector; // use Harris corner detector
    double k; // Harris detector free parameter
    int maxCornersOpticalFlow; // max corners for optical flow
    int minDistance; // minimum distance between corners
    int useShiTomasi; // use Shi-Tomasi corner detector
    int useOpticalFlow; // use optical flow
    int usePyrLK; // use pyramidal Lucas-Kanade optical flow
    int useFarneback; // use Farneback optical flow
    int useDenseOpticalFlow; // use dense optical flow
    int useSparseOpticalFlow; // use sparse optical flow
    int useOpticalFlowFarneback; // use Farneback optical flow
    int useOpticalFlowPyrLK; // use pyramidal Lucas-Kanade optical flow
    int useOpticalFlowDualTVL1; // use dual TV-L1 optical flow
    int useOpticalFlowSF; // use SuperFlow optical flow
    int useOpticalFlowBM; // use block matching optical flow
    int useOpticalFlowLK; // use Lucas-Kanade optical flow
    int useOpticalFlowHS; // use Horn-Schunck optical flow
    int useOpticalFlowLKFast; // use fast Lucas-Kanade optical flow
    int useOpticalFlowLKSparse; // use sparse Lucas-Kanade optical flow
    int useOpticalFlowLKDense; // use dense Lucas-Kanade optical flow
    int useOpticalFlowLKDualTVL1; // use dual TV-L1 Lucas-Kanade optical flow
    int useOpticalFlowLKSF; // use SuperFlow Lucas-Kanade optical flow
    int useOpticalFlowLKBM; // use block matching Lucas-Kanade optical flow
};
    

#endif // OPTICAL_FLOW_CONFIG_H