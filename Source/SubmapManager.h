#pragma once
#ifndef SUBMAP_MANAGER_H
#define SUBMAP_MANAGER_H

#include "SiftGPU/SIFTImageManager.h"
#include "CUDAImageManager.h"
#include "CUDACache.h"
#include "SBA.h"

#include "SiftGPU/CUDATimer.h"
#include "GlobalBundlingState.h"
#include "mLibCuda.h"

#define DEBUG_PRINT_MATCHING


class SiftGPU;
class SiftMatchGPU;

extern "C" void updateTrajectoryCU(
	float4x4* d_globalTrajectory, unsigned int numGlobalTransforms,
	float4x4* d_completeTrajectory, unsigned int numCompleteTransforms,
	float4x4* d_localTrajectories, unsigned int numLocalTransformsPerTrajectory, unsigned int numLocalTrajectories,
	int* d_imageInvalidateList);

extern "C" void initNextGlobalTransformCU(
	float4x4* d_globalTrajectory, unsigned int numGlobalTransforms,
	float4x4* d_localTrajectories, unsigned int numLocalTransformsPerTrajectory);

class SubmapManager {
public:
	SubmapManager();
	void init(unsigned int maxNumGlobalImages, unsigned int maxNumLocalImages, unsigned int maxNumKeysPerImage,
		unsigned int submapSize, const CUDAImageManager* imageManager, const RGBDSensor* sensor, unsigned int numTotalFrames = (unsigned int)-1);

	void setTotalNumFrames(unsigned int n) {
		m_numTotalFrames = n;
	}

	~SubmapManager();

	float4x4* getLocalTrajectoryGPU(unsigned int localIdx) const {
		return d_localTrajectories + localIdx * (m_submapSize + 1);
	}

	void invalidateImages(unsigned int startFrame, unsigned int endFrame = -1) {
		if (endFrame == -1) m_invalidImagesList[startFrame] = 0;
		else {
			for (unsigned int i = startFrame; i < endFrame; i++)
				m_invalidImagesList[i] = 0;
		}
	}
	void validateImages(unsigned int startFrame, unsigned int endFrame = -1) {
		if (endFrame == -1) m_invalidImagesList[startFrame] = 1;
		else {
			for (unsigned int i = startFrame; i < endFrame; i++)
				m_invalidImagesList[i] = 1;
		}
	}

	void switchLocal() {
		mutex_nextLocal.lock();
		std::swap(m_currentLocal, m_nextLocal);
		std::swap(m_currentLocalCache, m_nextLocalCache);
		mutex_nextLocal.unlock();
	}

	bool isLastLocalFrame(unsigned int curFrame) const { return (curFrame >= m_submapSize && (curFrame % m_submapSize) == 0); }
	unsigned int getCurrLocal(unsigned int curFrame) const {
		//const unsigned int curLocalIdx = (curFrame + 1 == m_numTotalFrames && (curFrame % m_submapSize != 0)) ? (curFrame / m_submapSize) : (curFrame / m_submapSize) - 1; // adjust for endframe
		//return curLocalIdx;
		return (std::max(curFrame, 1u) - 1) / m_submapSize;
	}

	void computeCurrentSiftTransform(unsigned int frameIdx, unsigned int localFrameIdx, unsigned int lastValidCompleteTransform) {
		const std::vector<int>& validImages = m_currentLocal->getValidImages();
		if (validImages[localFrameIdx] == 0) {
			m_currIntegrateTransform[frameIdx].setZero(-std::numeric_limits<float>::infinity());
			assert(frameIdx > 0);
			cutilSafeCall(cudaMemcpy(d_siftTrajectory + frameIdx, d_siftTrajectory + frameIdx - 1, sizeof(float4x4), cudaMemcpyDeviceToDevice));
			//cutilSafeCall(cudaMemcpy(d_currIntegrateTransform + frameIdx, &m_currIntegrateTransform[frameIdx], sizeof(float4x4), cudaMemcpyHostToDevice)); //TODO this is for debug only
		}
		else if (frameIdx > 0) {
			m_currentLocal->computeSiftTransformCU(d_completeTrajectory, lastValidCompleteTransform, d_siftTrajectory, frameIdx, localFrameIdx, d_currIntegrateTransform + frameIdx);
			cutilSafeCall(cudaMemcpy(&m_currIntegrateTransform[frameIdx], d_currIntegrateTransform + frameIdx, sizeof(float4x4), cudaMemcpyDeviceToHost));
		}
	}
	const mat4f& getCurrentIntegrateTransform(unsigned int frameIdx) const { return m_currIntegrateTransform[frameIdx]; }
	const std::vector<mat4f>& getAllIntegrateTransforms() const { return m_currIntegrateTransform; }

	void getCacheIntrinsics(float4x4& intrinsics, float4x4& intrinsicsInv);

	//! run sift for current local
	unsigned int runSIFT(unsigned int curFrame, float* d_intensitySIFT, const float* d_inputDepthFilt,
		unsigned int depthWidth, unsigned int depthHeight, const uchar4* d_inputColor,
		unsigned int colorWidth, unsigned int colorHeight, const float* d_inputDepthRaw);
	//! valid if at least frames 0, 1 valid
	bool isCurrentLocalValidChunk();
	unsigned int getNumNextLocalFrames();
	bool localMatchAndFilter(const float4x4& siftIntrinsicsInv) {
		//!!!debugging
		//if (m_global->getNumImages() >= 63 && m_global->getNumImages() <= 66) {
		//	setPrintMatchesDEBUG(true);
		//}
		//!!!debugging
		bool ret = matchAndFilter(true, m_currentLocal, m_currentLocalCache, siftIntrinsicsInv);
		//!!!debugging
		//if (m_currentLocal->getNumImages() == m_submapSize + 1 && m_global->getNumImages() == 66) {
		//	setPrintMatchesDEBUG(false);
		//}
		//!!!debugging
		return ret;
	}

	void copyToGlobalCache();
	void incrementGlobalCache() { m_globalCache->incrementCache(); }

	//! optimize local
	bool optimizeLocal(unsigned int curLocalIdx, unsigned int numNonLinIterations, unsigned int numLinIterations);
	int computeAndMatchGlobalKeys(unsigned int lastLocalSolved, const float4x4& siftIntrinsics, const float4x4& siftIntrinsicsInv);

	void tryRevalidation(unsigned int curGlobalFrame, const float4x4& siftIntrinsicsInv);

	void addInvalidGlobalKey();

	//! optimize global
	bool optimizeGlobal(unsigned int numFrames, unsigned int numNonLinIterations, unsigned int numLinIterations, bool isStart, bool isEnd, bool isScanDone);

	void invalidateLastGlobalFrame() {
		MLIB_ASSERT(m_global->getNumImages() > 1);
		m_global->invalidateFrame(m_global->getNumImages() - 1);
	}

	// update complete trajectory with new global trajectory info
	void updateTrajectory(unsigned int curFrame);

	const float4x4* getCompleteTrajectory() const { return d_completeTrajectory; }

	//debugging
	void saveGlobalSiftManagerAndCache(const std::string& prefix) const;
	void saveCompleteTrajectory(const std::string& filename, unsigned int numTransforms) const;
	void saveSiftTrajectory(const std::string& filename, unsigned int numTransforms) const;
	void printConvergence(const std::string& filename) const {
		m_SparseBundler.printConvergence(filename);
	}

	//! only debug 
	const SIFTImageManager* getCurrentLocalDEBUG() const { return m_currentLocal; }
	const SIFTImageManager* getGlobalDEBUG() const { return m_global; }
	void setPrintMatchesDEBUG(bool b) { _debugPrintMatches = b; }

	// to fake opt finish when no opt
	void resetDEBUG(bool initNextGlobal, int numLocalSolved, unsigned int curFrame) { //numLocalSolved == numGlobalFrames
		mutex_nextLocal.lock();
		if (initNextGlobal) {
			if (numLocalSolved >= 0) {
				float4x4 relativeTransform;
				MLIB_CUDA_SAFE_CALL(cudaMemcpy(&relativeTransform, getLocalTrajectoryGPU(numLocalSolved) + m_submapSize, sizeof(float4x4), cudaMemcpyDeviceToHost));
				float4x4 prevTransform;
				MLIB_CUDA_SAFE_CALL(cudaMemcpy(&prevTransform, d_globalTrajectory + numLocalSolved, sizeof(float4x4), cudaMemcpyDeviceToHost));
				float4x4 newTransform = prevTransform * relativeTransform;
				MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_globalTrajectory + numLocalSolved + 1, &newTransform, sizeof(float4x4), cudaMemcpyHostToDevice));
			}
			if (numLocalSolved > 0) {
				// update trajectory
				MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_imageInvalidateList, m_invalidImagesList.data(), sizeof(int)*curFrame, cudaMemcpyHostToDevice));

				updateTrajectoryCU(d_globalTrajectory, numLocalSolved,
					d_completeTrajectory, curFrame,
					d_localTrajectories, m_submapSize + 1, numLocalSolved,
					d_imageInvalidateList);
			}
		}
		finishLocalOpt();
		mutex_nextLocal.unlock();
	}
		//TODO fix this hack
	void setEndSolveGlobalDenseWeights();
	void setNumOptPerResidualRemoval(unsigned int n) { m_numOptPerResidualRemoval = n; }


private:

	//! sift matching
	bool matchAndFilter(bool isLocal, SIFTImageManager* siftManager, CUDACache* cudaCache, const float4x4& siftIntrinsicsInv); //!!!TODO FIX TIMING LOG

	void initSIFT(unsigned int widthSift, unsigned int heightSift);
	//! called when global locked
	void initializeNextGlobalTransform(bool useIdentity) {
		const unsigned int numGlobalFrames = m_global->getNumImages();
		MLIB_ASSERT(numGlobalFrames >= 1);
		if (useIdentity) {
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_globalTrajectory + numGlobalFrames, d_globalTrajectory + numGlobalFrames - 1, sizeof(float4x4), cudaMemcpyDeviceToDevice));
		}
		else {
			initNextGlobalTransformCU(d_globalTrajectory, numGlobalFrames, d_localTrajectories, m_submapSize + 1);
		}
	}
	//! called when nextlocal locked
	void finishLocalOpt() {
		m_nextLocal->reset();
		m_nextLocalCache->reset();
	}
	//! assumes nextlocal locked
	void saveOptToPointCloud(const std::string& filename, const CUDACache* cudaCache, const std::vector<int>& valid, const float4x4* d_transforms, unsigned int numFrames, bool saveFrameByFrame = false);
	void saveImPairToPointCloud(const std::string& prefix, const CUDACache* cudaCache, const float4x4* d_transforms, const vec2ui& imageIndices, const mat4f& transformCurToPrv = mat4f::zero()) const;

	//*********** SIFT *******************
	SiftGPU*				m_sift;
	SiftMatchGPU*			m_siftMatcher;
	//************ SUBMAPS ********************
	SBA						m_SparseBundler;

	std::mutex mutex_nextLocal;
	std::mutex m_mutexMatcher;

	CUDACache*			m_currentLocalCache;
	SIFTImageManager*	m_currentLocal;

	CUDACache*			m_nextLocalCache;
	SIFTImageManager*	m_nextLocal;

	CUDACache*			m_globalCache;
	SIFTImageManager*	m_global;

	//!!!TODO HERE
	CUDACache*			m_optLocalCache;
	SIFTImageManager*	m_optLocal;
	//*********** TRAJECTORIES ************
	float4x4* d_globalTrajectory;
	float4x4* d_completeTrajectory;
	float4x4* d_localTrajectories;
	std::vector<std::vector<int>> m_localTrajectoriesValid;

	float4x4*	 d_siftTrajectory; // frame-to-frame sift tracking for all frames in sequence
	//************************************

	std::vector<unsigned int>	m_invalidImagesList;
	int*						d_imageInvalidateList; // tmp for updateTrajectory //TODO just to update trajectory on CPU

	float4x4*					d_currIntegrateTransform;
	std::vector<mat4f>			m_currIntegrateTransform;

	unsigned int m_numTotalFrames;
	unsigned int m_submapSize;
	unsigned int m_numOptPerResidualRemoval;

#ifdef DEBUG_PRINT_MATCHING
	bool _debugPrintMatches;
#endif
};

#endif