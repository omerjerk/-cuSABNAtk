/***
 *    $Id$
 **
 *    File: GPUCounter.hpp
 *    Created: Oct 22, 2016
 *
 *    Authors: Marc Greenbaum <marcgree@buffalo.edu>
 *             Mohammad Umair <m39@buffalo.edu>
 *    Copyright (c) 2015-2019 SCoRe Group http://www.score-group.org/
 *    Distributed under the MIT License.
 *    See accompanying file LICENSE.
 */

#ifndef GPU_COUNTER_HPP
#define GPU_COUNTER_HPP

#define CUDA_API_PER_THREAD_DEFAULT_STREAM

#include <cinttypes>
#include <vector>
#include <atomic>

#include <cuda_runtime.h>

#include "bit_util.hpp"
#include "gpu_util.cuh"


// TODO: check if these should be hardware dependent
static const int MAX_COUNTS_PER_QUERY = 1024;


template <int N> class GPUCounter {
public:
    using set_type = uint_type<N>;

    ~GPUCounter() {
        cudaFree(&base_->bvPtr_);
        delete[] nodeList_;
        delete base_;
    } // GPUCounter

    int n() const { return base_->n_; }

    int m() const { return base_->m_; }

    int r(int i) const { return base_->nodeList_[i].r_; }

    bool is_reorderable() { return false; }

    // FIXME: consider cases when |xa_vect| is greater than 1
    //        SABNAtk assumes that apply() is const, to allow CUDA concurrency, we are changing that
    template <typename score_functor>
    void apply(const std::vector<int>& xa_vect, const std::vector<int>& pa_vect, std::vector<score_functor>& F) {
        std::vector<int> xi(1 + pa_vect.size());
        xi[0] = xa_vect[0];

        std::vector<uint64_t> arities;
        std::vector<uint64_t> aritiesPrefixProd;
        std::vector<uint64_t> aritiesPrefixSum;

        // build arities list
        arities.push_back(r(xi[0]));
        aritiesPrefixProd.push_back(1);
        aritiesPrefixSum.push_back(aritiesPrefixSumGlobal_[xi[0]]);

        int arity = 0;

        for (int i = 0; i < pa_vect.size(); i++) {
            xi[i + 1] = pa_vect[i];
            arity = r(xi[i + 1]);

            arities.push_back(arity);
            aritiesPrefixProd.push_back(aritiesPrefixProd[i] * arities[i]);
            aritiesPrefixSum.push_back(aritiesPrefixSumGlobal_[xi[i + 1]]);
        } // for i

        int maxConfigCount = aritiesPrefixProd[aritiesPrefixProd.size() - 1] * arities[arities.size() - 1];
        // printf("query count = %d\n", queryCountPtr->load());
        int streamId = queryCountPtr->load() % 32;
        // printf("done with kernel sid = %d\n", streamId);

        copyAritiesToDevice(streamId, arities, aritiesPrefixProd, aritiesPrefixSum);

        // call gpu kernel on each subgroup
        cudaCallBlockCount(65535,                          // device limit, not used
                           1024,                           // device limit, not used
                           base_->bitvectorSize_,          // number of words in each bitvector
                           arities.size(),                 // number of variables in one config
                           maxConfigCount,                 // number of configurations
                           base_->nodeList_[0].bitvectors, // starting address of our data
                           resultList_,                    // results array for Nijk
                           resultListPa_,                  // results array for Nij
                           0,
                           streamId);

        for (int i = 0; i < maxConfigCount; ++i) {
            if (resultList_[(streamId * MAX_COUNTS_PER_QUERY) + i] > 0) {
                F[0](resultList_[(streamId * MAX_COUNTS_PER_QUERY) + i], resultListPa_[(streamId * MAX_COUNTS_PER_QUERY) + i]);
            }
        }

        *queryCountPtr += 1;
    } // apply

private:
    int m_bvSize__() const { return this->base_.bitvectorSize_; }

    // given the variable number and its state, this function returns the GPU address
    // where that particular bitvector starts
    uint64_t* m_getBvPtr__(int pNode, int pState) const {
        uint64_t* resultPtr = nullptr;
        if (pNode < n() && pState < r(pNode)) {
            resultPtr = base_->nodeList_[pNode].bitvectors + (pState * base_->bitvectorSize_);
        }
        return resultPtr;
    } // m_getBvPtr__

    struct node {
        int id;
        int r_;
        uint64_t* bitvectors;
    }; // struct node

    struct base {
        int n_;
        int m_;
        int bitvectorSize_;
        uint64_t* bvPtr_;
        node* nodeList_;
    }; // struct base

    // minimal description of the input data
    base* base_ = nullptr;

    // contains GPU memory addresses where each particular value of xi starts
    node* nodeList_ = nullptr;

    std::vector<int> aritiesPrefixSumGlobal_;

    // results of each configuration of the given query
    uint64_t* resultList_;
    uint64_t* resultListPa_;

    std::atomic_int* queryCountPtr;

    // MAX_NUM_STREAMS should be runtime parameter decided on data
    // and with respect to user provided hints
    int MAX_NUM_STREAMS_;
    std::vector<cudaStream_t> streams;

    template <int M, typename Iter>
    friend GPUCounter<M> create_GPUCounter(int, int, Iter);
}; // class GPUCounter


// TODO: add option to select which CUDA device to use
//       we should be also configuring number of streams automatically
template <int N, typename Iter> GPUCounter<N> create_GPUCounter(int n, int m, Iter it) {
    int devicesCount = 0;
    cudaGetDeviceCount(&devicesCount);

    if (devicesCount == 0) throw std::runtime_error("no CUDA capable devices found");

    GPUCounter<N> p;

    int indices[256];
    int temp = 0;
    int size = 0;

    Iter temp_it = it;
    int bitvectorCount = 0;

    // we use 64bit words
    // you will see constants: shifting by 6, and 63 and 64
    int bitvectorSize = (m + 63) / 64;

    p.base_ = new typename GPUCounter<N>::base;
    p.base_->n_ = n;
    p.base_->m_ = m;
    p.base_->bitvectorSize_ = bitvectorSize;
    p.base_->nodeList_ = new typename GPUCounter<N>::node[n];

    // determine |ri| of each Xi
    // FIXME: report and terminate if arities exceed our GPU capability
    for (int xi = 0; xi < n; ++xi) {
        size = 0;
        std::fill_n(indices, 256, -1);

        for (int j = 0; j < m; ++j) {
            temp = *temp_it++;
            if (indices[temp] == -1) indices[temp] = size++;
        }

        p.base_->nodeList_[xi].r_ = size;
        bitvectorCount += size;
    } // for xi

    int bitvectorWordCount = bitvectorCount * bitvectorSize;

    uint64_t* bvPtr = nullptr;
    cudaMalloc(&bvPtr, sizeof(uint64_t) * bitvectorWordCount);

    p.base_->bvPtr_ = bvPtr;

    // set bitvector addresses (device addrs) in node list (host mem)
    int offset = 0;

    for (int xi = 0; xi < n; ++xi) {
        p.base_->nodeList_[xi].bitvectors = bvPtr + offset;
        offset += p.base_->nodeList_[xi].r_ * bitvectorSize;
        if (!xi) {
            p.aritiesPrefixSumGlobal_.push_back(0);
        } else {
            p.aritiesPrefixSumGlobal_.push_back(p.aritiesPrefixSumGlobal_[p.aritiesPrefixSumGlobal_.size() - 1] + p.base_->nodeList_[xi - 1].r_);
        }
    } // for xi

    // build bitvectors for each Xi and copy into device
    uint64_t* tempBvPtr = new uint64_t[bitvectorWordCount];
    memset(tempBvPtr, 0, sizeof(uint64_t) * bitvectorWordCount);

    temp_it = it;
    offset = 0;

    for (int xi = 0; xi < n; ++xi) {
        for (int j = 0; j < m; ++j) {
            temp = *temp_it++;
            int bv_index = temp;
            uint64_t& bitvector = tempBvPtr[offset + (bv_index * bitvectorSize + (j >> 6))]; // find the block
            bitvector |= (1L << (j & 63)); // set the bit
        }
        offset += p.base_->nodeList_[xi].r_ * bitvectorSize;
    }

    cudaMemcpy(bvPtr, tempBvPtr, sizeof(uint64_t) * bitvectorWordCount, cudaMemcpyHostToDevice);
    delete[] tempBvPtr;

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0); //assuming that there is just one device
    //The max number of concurrent kernels on compute capability 7.0+ is 128.
    //However, limiting it to 64 because I am a bit skeptical that we may actually end up losing
    //performance with 128 streams
    if (prop.concurrentKernels > 64) {
        p.MAX_NUM_STREAMS_ = 64;
    } else {
        p.MAX_NUM_STREAMS_ = prop.concurrentKernels;
    }

    // expected size = (number of configurations in the query) * sizeof(uint64_t)
    cudaMallocManaged(&p.resultList_, sizeof(uint64_t) * MAX_COUNTS_PER_QUERY * p.MAX_NUM_STREAMS_);
    cudaMallocManaged(&p.resultListPa_, sizeof(uint64_t) * MAX_COUNTS_PER_QUERY * p.MAX_NUM_STREAMS_);

    p.streams.resize(p.MAX_NUM_STREAMS_);
    p.queryCountPtr = new std::atomic_int(0);

    for (int i = 0; i < p.MAX_NUM_STREAMS_; ++i) {
        cudaStreamCreate(&p.streams[i]);
    }

    cudaDeviceSynchronize();

    return p;
} // create_GPUCounter

#endif // GPU_COUNTER_HPP
