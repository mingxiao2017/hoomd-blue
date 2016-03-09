/*
Highly Optimized Object-oriented Many-particle Dynamics -- Blue Edition
(HOOMD-blue) Open Source Software License Copyright 2009-2015 The Regents of
the University of Michigan All rights reserved.

HOOMD-blue may contain modifications ("Contributions") provided, and to which
copyright is held, by various Contributors who have granted The Regents of the
University of Michigan the right to modify and/or distribute such Contributions.

You may redistribute, use, and create derivate works of HOOMD-blue, in source
and binary forms, provided you abide by the following conditions:

* Redistributions of source code must retain the above copyright notice, this
list of conditions, and the following disclaimer both in the code and
prominently in any materials provided with the distribution.

* Redistributions in binary form must reproduce the above copyright notice, this
list of conditions, and the following disclaimer in the documentation and/or
other materials provided with the distribution.

* All publications and presentations based on HOOMD-blue, including any reports
or published results obtained, in whole or in part, with HOOMD-blue, will
acknowledge its use according to the terms posted at the time of submission on:
http://codeblue.umich.edu/hoomd-blue/citations.html

* Any electronic documents citing HOOMD-Blue will link to the HOOMD-Blue website:
http://codeblue.umich.edu/hoomd-blue/

* Apart from the above required attributions, neither the name of the copyright
holder nor the names of HOOMD-blue's contributors may be used to endorse or
promote products derived from this software without specific prior written
permission.

Disclaimer

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND/OR ANY
WARRANTIES THAT THIS SOFTWARE IS FREE OF INFRINGEMENT ARE DISCLAIMED.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Maintainer: joaander

#include "TwoStepBDGPU.cuh"
#include "saruprngCUDA.h"
#include "VectorMath.h"
#include "HOOMDMath.h"

#include <assert.h>

/*! \file TwoSteBDGPU.cu
    \brief Defines GPU kernel code for Brownian integration on the GPU. Used by TwoStepBDGPU.
*/

//! Shared memory array for gpu_langevin_step_two_kernel()
extern __shared__ Scalar s_gammas[];

//! Takes the second half-step forward in the Langevin integration on a group of particles with
/*! \param d_pos array of particle positions and types
    \param d_vel array of particle positions and masses
    \param d_image array of particle images
    \param box simulation box
    \param d_diameter array of particle diameters
    \param d_tag array of particle tags
    \param d_group_members Device array listing the indicies of the mebers of the group to integrate
    \param group_size Number of members in the group
    \param d_net_force Net force on each particle
    \param d_gamma_r List of per-type gamma_rs (rotational drag coeff.)
    \param d_orientation Device array of orientation quaternion
    \param d_torque Device array of net torque on each particle
    \param d_inertia Device array of moment of inertial of each particle
    \param d_angmom Device array of transformed angular momentum quaternion of each particle (see online documentation)
    \param d_gamma List of per-type gammas
    \param n_types Number of particle types in the simulation
    \param use_lambda If true, gamma = lambda * diameter
    \param lambda Scale factor to convert diameter to lambda (when use_lambda is true)
    \param timestep Current timestep of the simulation
    \param seed User chosen random number seed
    \param T Temperature set point
    \param aniso If set true, the system would go through rigid body updates for its orientation 
    \param deltaT Amount of real time to step forward in one time step
    \param D Dimensionality of the system
    \param d_noiseless_t If set true, there will be no translational noise (random force)
    \param d_noiseless_r If set true, there will be no rotational noise (random torque)

    This kernel is implemented in a very similar manner to gpu_nve_step_one_kernel(), see it for design details.

    Random number generation is done per thread with Saru's 3-seed constructor. The seeds are, the time step,
    the particle tag, and the user-defined seed.

    This kernel must be launched with enough dynamic shared memory per block to read in d_gamma
*/
extern "C" __global__
void gpu_brownian_step_one_kernel(Scalar4 *d_pos,
                                  Scalar4 *d_vel,
                                  int3 *d_image,
                                  const BoxDim box,
                                  const Scalar *d_diameter,
                                  const unsigned int *d_tag,
                                  const unsigned int *d_group_members,
                                  const unsigned int group_size,
                                  const Scalar4 *d_net_force,
                                  const Scalar *d_gamma_r,
                                  Scalar4 *d_orientation,
                                  Scalar4 *d_torque,
                                  const Scalar3 *d_inertia,
                                  Scalar4 *d_angmom,
                                  const Scalar *d_gamma,
                                  const unsigned int n_types,
                                  const bool use_lambda,
                                  const Scalar lambda,
                                  const unsigned int timestep,
                                  const unsigned int seed,
                                  const Scalar T,
                                  const bool aniso,
                                  const Scalar deltaT,
                                  unsigned int D, 
                                  const bool d_noiseless_t,
                                  const bool d_noiseless_r)
    {
    if (!use_lambda)
        {
        // read in the gamma (1 dimensional array), stored in s_gammas[0: n_type] (Pythonic convention)
        for (int cur_offset = 0; cur_offset < n_types; cur_offset += blockDim.x)
            {
            if (cur_offset + threadIdx.x < n_types)
                s_gammas[cur_offset + threadIdx.x] = d_gamma[cur_offset + threadIdx.x];
            }
        __syncthreads();
        }
    
    // read in the gamma_r, stored in s_gammas[n_type: 2 * n_type], which is s_gamma_r[0:n_type]
        
    Scalar * s_gammas_r = s_gammas + n_types;
    for (int cur_offset = 0; cur_offset < n_types; cur_offset += blockDim.x)
        {
        if (cur_offset + threadIdx.x < n_types)
            s_gammas_r[cur_offset + threadIdx.x] = d_gamma_r[cur_offset + threadIdx.x];
        }
    __syncthreads(); 
    
    // determine which particle this thread works on (MEM TRANSFER: 4 bytes)
    int group_idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (group_idx < group_size)
        {
        // determine the particle to work on
        unsigned int idx = d_group_members[group_idx];
        Scalar4 postype = d_pos[idx];
        Scalar4 vel = d_vel[idx];
        Scalar4 net_force = d_net_force[idx];
        int3 image = d_image[idx];

        // read in the tag of our particle.
        unsigned int ptag = d_tag[idx];

        // compute the random force
        SaruGPU saru(ptag, timestep + seed, 0x9977665);
        Scalar rx = saru.s<Scalar>(-1,1);
        Scalar ry = saru.s<Scalar>(-1,1);
        Scalar rz =  saru.s<Scalar>(-1,1);

        // calculate the magnitude of the random force
        Scalar gamma;
        if (use_lambda)
            {
            // determine gamma from diameter
            gamma = lambda*d_diameter[idx];
            }
        else
            {
            // determine gamma from type
            unsigned int typ = __scalar_as_int(postype.w);
            gamma = s_gammas[typ];
            }

        // compute the bd force (the extra factor of 3 is because <rx^2> is 1/3 in the uniform -1,1 distribution
        // it is not the dimensionality of the system
        Scalar coeff = fast::sqrt(Scalar(3.0)*Scalar(2.0)*gamma*T/deltaT);
        if (d_noiseless_t) 
            coeff = Scalar(0.0);
        Scalar Fr_x = rx*coeff;
        Scalar Fr_y = ry*coeff;
        Scalar Fr_z = rz*coeff;

        if (D < 3)
            Fr_z = Scalar(0.0);

        // update position
        postype.x += (net_force.x + Fr_x) * deltaT / gamma;
        postype.y += (net_force.y + Fr_y) * deltaT / gamma;
        postype.z += (net_force.z + Fr_z) * deltaT / gamma;

        // particles may have been moved slightly outside the box by the above steps, wrap them back into place
        box.wrap(postype, image);

        // draw a new random velocity for particle j
        Scalar mass = vel.w;
        Scalar sigma = fast::sqrt(T/mass);
        vel.x = gaussian_rng(saru, sigma);
        vel.y = gaussian_rng(saru, sigma);
        if (D > 2)
            vel.z = gaussian_rng(saru, sigma);
        else
            vel.z = 0;

        // write out data
        d_pos[idx] = postype;
        d_vel[idx] = vel;
        d_image[idx] = image;
        
        // rotational random force and orientation quaternion updates
        if (aniso)
            {
            unsigned int type_r = __scalar_as_int(d_pos[idx].w);
            
            // gamma_r is stored in the second half of s_gammas a.k.a s_gammas_r
            Scalar gamma_r = s_gammas_r[type_r];
            if (gamma_r > 0)
                {
                vec3<Scalar> p_vec;
                quat<Scalar> q(d_orientation[idx]);
                vec3<Scalar> t(d_torque[idx]);
                vec3<Scalar> I(d_inertia[idx]);
                
                // check if the shape is degenerate
                bool x_zero, y_zero, z_zero;
                x_zero = (I.x < EPSILON); y_zero = (I.y < EPSILON); z_zero = (I.z < EPSILON);  
                    
                Scalar sigma_r = fast::sqrt(Scalar(2.0)*gamma_r*T/deltaT);
                if (d_noiseless_r) 
                    sigma_r = Scalar(0.0);

                // original Gaussian random torque
                // Gaussian random distribution is preferred in terms of preserving the exact math
                vec3<Scalar> bf_torque;
                bf_torque.x = gaussian_rng(saru, sigma_r); 
                bf_torque.y = gaussian_rng(saru, sigma_r); 
                bf_torque.z = gaussian_rng(saru, sigma_r);
                
                if (x_zero) bf_torque.x = 0;
                if (y_zero) bf_torque.y = 0;
                if (z_zero) bf_torque.z = 0;
                
                // use the damping by gamma_r and rotate back to lab frame 
                // For Future Updates: take special care when have anisotropic gamma_r 
                bf_torque = rotate(q, bf_torque);
                if (D < 3)
                    {
                    bf_torque.x = 0;
                    bf_torque.y = 0;
                    t.x = 0;
                    t.y = 0;
                    }

                // do the integration for quaternion
                q += Scalar(0.5) * deltaT * ((t + bf_torque) / gamma_r) * q ;               
                q = q * (Scalar(1.0) / slow::sqrt(norm2(q)));
                d_orientation[idx] = quat_to_scalar4(q);
                
                // draw a new random ang_mom for particle j in body frame
                p_vec.x = gaussian_rng(saru, fast::sqrt(T * I.x));
                p_vec.y = gaussian_rng(saru, fast::sqrt(T * I.y));
                p_vec.z = gaussian_rng(saru, fast::sqrt(T * I.z));
                if (x_zero) p_vec.x = 0;
                if (y_zero) p_vec.y = 0;
                if (z_zero) p_vec.z = 0;
                
                // !! Note this ang_mom isn't well-behaving in 2D, 
                // !! because may have effective non-zero ang_mom in x,y
                
                // store ang_mom quaternion
                quat<Scalar> p = Scalar(2.0) * q * p_vec;
                d_angmom[idx] = quat_to_scalar4(p);
                }
            }
        }
    }

/*! \param d_pos array of particle positions and types
    \param d_vel array of particle positions and masses
    \param d_image array of particle images
    \param box simulation box
    \param d_diameter array of particle diameters
    \param d_tag array of particle tags
    \param d_group_members Device array listing the indicies of the mebers of the group to integrate
    \param group_size Number of members in the group
    \param d_net_force Net force on each particle
    \param d_gamma_r List of per-type gamma_rs (rotational drag coeff.)
    \param d_orientation Device array of orientation quaternion
    \param d_torque Device array of net torque on each particle
    \param d_inertia Device array of moment of inertial of each particle
    \param d_angmom Device array of transformed angular momentum quaternion of each particle (see online documentation)
    \param langevin_args Collected arguments for gpu_brownian_step_one_kernel()
    \param aniso If set true, the system would go through rigid body updates for its orientation 
    \param deltaT Amount of real time to step forward in one time step
    \param D Dimensionality of the system
    \param d_noiseless_t If set true, there will be no translational noise (random force)
    \param d_noiseless_r If set true, there will be no rotational noise (random torque)
    
    This is just a driver for gpu_brownian_step_one_kernel(), see it for details.
*/
cudaError_t gpu_brownian_step_one(Scalar4 *d_pos,
                                  Scalar4 *d_vel,
                                  int3 *d_image,
                                  const BoxDim& box,
                                  const Scalar *d_diameter,
                                  const unsigned int *d_tag,
                                  const unsigned int *d_group_members,
                                  const unsigned int group_size,
                                  const Scalar4 *d_net_force,
                                  const Scalar *d_gamma_r,
                                  Scalar4 *d_orientation,
                                  Scalar4 *d_torque,
                                  const Scalar3 *d_inertia,
                                  Scalar4 *d_angmom,
                                  const langevin_step_two_args& langevin_args,
                                  const bool aniso,
                                  const Scalar deltaT,
                                  const unsigned int D, 
                                  const bool d_noiseless_t,
                                  const bool d_noiseless_r)
    {

    // setup the grid to run the kernel
    dim3 grid(langevin_args.num_blocks, 1, 1);
    dim3 grid1(1, 1, 1);
    dim3 threads(langevin_args.block_size, 1, 1);
    dim3 threads1(256, 1, 1);

    // run the kernel
    gpu_brownian_step_one_kernel<<< grid, threads, max( (unsigned int)(sizeof(Scalar)*langevin_args.n_types) * 2,
                                                        (unsigned int)(langevin_args.block_size*sizeof(Scalar))
                                                      )>>>
                                (d_pos,
                                 d_vel,
                                 d_image,
                                 box,
                                 d_diameter,
                                 d_tag,
                                 d_group_members,
                                 group_size,
                                 d_net_force,
                                 d_gamma_r,
                                 d_orientation,
                                 d_torque,
                                 d_inertia,
                                 d_angmom,
                                 langevin_args.d_gamma,
                                 langevin_args.n_types,
                                 langevin_args.use_lambda,
                                 langevin_args.lambda,
                                 langevin_args.timestep,
                                 langevin_args.seed,
                                 langevin_args.T,
                                 aniso,
                                 deltaT,
                                 D, 
                                 d_noiseless_t,
                                 d_noiseless_r);

    return cudaSuccess;
    }
