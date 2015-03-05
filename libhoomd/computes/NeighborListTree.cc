/*
Highly Optimized Object-oriented Many-particle Dynamics -- Blue Edition
(HOOMD-blue) Open Source Software License Copyright 2009-2014 The Regents of
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

// Maintainer: mphoward

/*! \file NeighborListTree.cc
    \brief Defines NeighborListTree
*/

#include "NeighborListTree.h"
#include "SystemDefinition.h"

#include <boost/bind.hpp>
#include <boost/python.hpp>
using namespace boost;
using namespace boost::python;

#ifdef ENABLE_MPI
#include "Communicator.h"
#endif

NeighborListTree::NeighborListTree(boost::shared_ptr<SystemDefinition> sysdef,
                                       Scalar r_cut,
                                       Scalar r_buff)
    : NeighborList(sysdef, r_cut, r_buff), m_type_changed(true), m_box_changed(true), m_max_num_changed(true),
      m_remap_particles(true), m_n_images(0)
    {
    m_exec_conf->msg->notice(5) << "Constructing NeighborListTree" << endl;

    m_boxchange_connection = m_pdata->connectBoxChange(bind(&NeighborListTree::slotBoxChanged, this));
    m_max_numchange_conn = m_pdata->connectMaxParticleNumberChange(bind(&NeighborListTree::slotMaxNumChanged, this));
    m_sort_conn = m_pdata->connectParticleSort(bind(&NeighborListTree::slotRemapParticles, this));
    }

NeighborListTree::~NeighborListTree()
    {
    m_exec_conf->msg->notice(5) << "Destroying NeighborListTree" << endl;
    m_boxchange_connection.disconnect();
    m_max_numchange_conn.disconnect();
    m_sort_conn.disconnect();
    }

void NeighborListTree::buildNlist(unsigned int timestep)
    {   
    // allocate the memory as needed and sort particles
    setupTree();
    
    // build the trees 
    buildTree();
    
    // now walk the trees
    traverseTree();
    }

//! manage the malloc of the AABB list
void NeighborListTree::setupTree()
    {
    if (m_max_num_changed)
        {
        m_aabbs.resize(m_pdata->getMaxN());
        m_map_p_global_tree.resize(m_pdata->getMaxN());
        
        m_max_num_changed = false;
        }
    
    if (m_type_changed)
        {
        m_aabb_trees.resize(m_pdata->getNTypes());
        m_num_per_type.resize(m_pdata->getNTypes(), 0);
        m_type_head.resize(m_pdata->getNTypes(), 0);
        
        slotRemapParticles();
        
        m_type_changed = false;
        }
    
    if (m_remap_particles)
        {
        mapParticlesByType();
        m_remap_particles = false;
        }
    
    if (m_box_changed)
        {
        updateImageVectors();
        m_box_changed = false;
        }
    }

//! get the number of particles by type (including ghost particles)
void NeighborListTree::mapParticlesByType()
    {
    if (this->m_prof) this->m_prof->push("Histogram");
    
    // clear out counters
    unsigned int n_types = m_pdata->getNTypes();
    for (unsigned int i=0; i < n_types; ++i)
        {
        m_num_per_type[i] = 0;
        }
    
    // histogram the particles
    unsigned int n_local = m_pdata->getN() + m_pdata->getNGhosts();
    ArrayHandle<Scalar4> h_postype(m_pdata->getPositions(), access_location::host, access_mode::read);
    for (unsigned int i=0; i < n_local; ++i)
        {
        unsigned int my_type = __scalar_as_int(h_postype.data[i].w);
        m_map_p_global_tree[i] = m_num_per_type[my_type]; // global id i is particle num_per_type after head of my_type
        ++m_num_per_type[my_type];
        }
    
    // set the head for each type in m_aabbs
    unsigned int local_head = 0;
    for (unsigned int i=0; i < n_types; ++i)
        {
        m_type_head[i] = local_head;
        local_head += m_num_per_type[i];
        }
        
    if (this->m_prof) this->m_prof->pop();
    }

//! compute the image vectors for translating periodically around tree
void NeighborListTree::updateImageVectors()
    {
    const BoxDim& box = m_pdata->getBox();
    uchar3 periodic = box.getPeriodic();

    // check that rcut fits in the box
    Scalar3 nearest_plane_distance = box.getNearestPlaneDistance();
    Scalar rmax = m_r_cut_max + m_r_buff;
    if ((periodic.x && nearest_plane_distance.x <= rmax * 2.0) ||
        (periodic.y && nearest_plane_distance.y <= rmax * 2.0) ||
        (this->m_sysdef->getNDimensions() == 3 && periodic.z && nearest_plane_distance.z <= rmax * 2.0))
        {
        m_exec_conf->msg->error() << "nlist: Simulation box is too small! Particles would be interacting with themselves." << endl;
        throw runtime_error("Error updating neighborlist bins");
        }

    // now compute the image vectors
    // each dimension increases by one power of 3
    unsigned int n_dim_periodic = (periodic.x + periodic.y + periodic.z);
    m_n_images = 1;
    for (unsigned int dim = 0; dim < n_dim_periodic; ++dim)
        {
        m_n_images *= 3;
        }
    
    // reallocate memory if necessary
    if (m_n_images > m_image_list.size())
        {
        m_image_list.resize(m_n_images);
        }
    
    vec3<Scalar> latt_a = vec3<Scalar>(box.getLatticeVector(0));
    vec3<Scalar> latt_b = vec3<Scalar>(box.getLatticeVector(1));
    vec3<Scalar> latt_c = vec3<Scalar>(box.getLatticeVector(2));
    
    // there is always at least 1 image, which we put as our first thing to look at
    m_image_list[0] = vec3<Scalar>(0.0, 0.0, 0.0);
    
    // iterate over all other combinations of images, skipping those that are 
    unsigned int n_images = 1;
    for (int i=-1; i <= 1 && n_images < m_n_images; ++i)
        {
        for (int j=-1; j <= 1 && n_images < m_n_images; ++j)
            {
            for (int k=-1; k <= 1 && n_images < m_n_images; ++k)
                {
                if (!(i == 0 && j == 0 && k == 0))
                    {
                    // skip any periodic images if we don't have periodicity
                    if (i != 0 && !periodic.x) continue;
                    if (j != 0 && !periodic.y) continue;
                    if (k != 0 && !periodic.z) continue;
                    
                    m_image_list[n_images] = Scalar(i) * latt_a + Scalar(j) * latt_b + Scalar(k) * latt_c;
                    ++n_images;
                    }
                }
            }
        }
    }

void NeighborListTree::buildTree()
    {
    m_exec_conf->msg->notice(4) << "Building AABB tree: " << m_pdata->getN() << " ptls "
                                << m_pdata->getNGhosts() << " ghosts" << endl;
                                
    // do the build
    if (this->m_prof) this->m_prof->push("Build");
    ArrayHandle<Scalar4> h_postype(m_pdata->getPositions(), access_location::host, access_mode::read);
    
    for (unsigned int i=0; i < m_pdata->getN()+m_pdata->getNGhosts(); ++i)
        {
        // make a point particle AABB
        vec3<Scalar> my_pos(h_postype.data[i]);
        unsigned int my_type = __scalar_as_int(h_postype.data[i].w);
        unsigned int my_aabb_idx = m_type_head[my_type] + m_map_p_global_tree[i];
        m_aabbs[my_aabb_idx] = AABB(my_pos,i);
        }
    
    for (unsigned int i=0; i < m_pdata->getNTypes(); ++i) 
        {
        m_aabb_trees[i].buildTree(&m_aabbs[0] + m_type_head[i], m_num_per_type[i]);
        }
    if (this->m_prof) this->m_prof->pop();
    
    }

void NeighborListTree::traverseTree()
    {
    if (this->m_prof) this->m_prof->push("Traverse");
    
    // acquire particle data
    ArrayHandle<Scalar4> h_postype(m_pdata->getPositions(), access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_body(m_pdata->getBodies(), access_location::host, access_mode::read);
    ArrayHandle<Scalar> h_diameter(m_pdata->getDiameters(), access_location::host, access_mode::read);
    
    ArrayHandle<Scalar> h_r_cut(m_r_cut, access_location::host, access_mode::read);
    
    // neighborlist data
    ArrayHandle<unsigned int> h_head_list(m_head_list, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_Nmax(m_Nmax, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_conditions(m_conditions, access_location::host, access_mode::readwrite);
    ArrayHandle<unsigned int> h_nlist(m_nlist, access_location::host, access_mode::overwrite);
    ArrayHandle<unsigned int> h_n_neigh(m_n_neigh, access_location::host, access_mode::overwrite);
    
    // Loop over all particles
    for (unsigned int i=0; i < m_pdata->getN(); ++i)
        {
        // read in the current position and orientation
        Scalar4 postype_i = h_postype.data[i];
        vec3<Scalar> pos_i = vec3<Scalar>(postype_i);
        unsigned int type_i = __scalar_as_int(postype_i.w);
        unsigned int body_i = h_body.data[i];
        
        unsigned int Nmax_i = h_Nmax.data[type_i];
        unsigned int nlist_head_i = h_head_list.data[i];
        
        unsigned int n_neigh_i = 0;
        for (unsigned int cur_pair_type=0; cur_pair_type < m_pdata->getNTypes(); ++cur_pair_type)
            {
            // Check primary box
            Scalar r_cut_i = h_r_cut.data[m_typpair_idx(type_i,cur_pair_type)]+m_r_buff;
            Scalar r_cutsq_i = r_cut_i*r_cut_i;
            AABBTree *cur_aabb_tree = &m_aabb_trees[cur_pair_type];

            for (unsigned int cur_image = 0; cur_image < m_n_images; ++cur_image)
                {
                vec3<Scalar> pos_i_image = pos_i + m_image_list[cur_image];
                AABB aabb = AABB(pos_i_image, r_cut_i);

                // stackless search
                for (unsigned int cur_node_idx = 0; cur_node_idx < cur_aabb_tree->getNumNodes(); ++cur_node_idx)
                    {
                    if (overlap(cur_aabb_tree->getNodeAABB(cur_node_idx), aabb))
                        {                        
                        if (cur_aabb_tree->isNodeLeaf(cur_node_idx))
                            {
                            for (unsigned int cur_p = 0; cur_p < cur_aabb_tree->getNodeNumParticles(cur_node_idx); ++cur_p)
                                {
                                // neighbor j
                                unsigned int j = cur_aabb_tree->getNodeParticleTag(cur_node_idx, cur_p);
                                
                                // skip self-interaction always
                                if (i == j)
                                    continue;
                                bool excluded = (i == j);

                                if (m_filter_body && body_i != NO_BODY)
                                    excluded = excluded | (body_i == h_body.data[j]);
                                    
                                if (!excluded)
                                    {
                                    // compute distance
                                    Scalar4 postype_j = h_postype.data[j];
                                    Scalar3 drij = make_scalar3(postype_j.x,postype_j.y,postype_j.z)
                                                   - vec_to_scalar3(pos_i_image);
                                    Scalar dr_sq = dot(drij,drij);

                                    if (dr_sq <= r_cutsq_i)
                                        {
                                        if (m_storage_mode == full || i < j)
                                            {
                                            if (n_neigh_i < Nmax_i)
                                                h_nlist.data[nlist_head_i + n_neigh_i] = j;
                                            else
                                                h_conditions.data[type_i] = max(h_conditions.data[type_i], n_neigh_i+1);

                                            ++n_neigh_i;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    else
                        {
                        // skip ahead
                        cur_node_idx += cur_aabb_tree->getNodeSkip(cur_node_idx);
                        }
                    } // end stackless search
                } // end loop over images
            } // end loop over pair types
            h_n_neigh.data[i] = n_neigh_i;
        } // end loop over particles
    
    if (this->m_prof) this->m_prof->pop();
    }

void export_NeighborListTree()
    {
    class_<NeighborListTree, boost::shared_ptr<NeighborListTree>, bases<NeighborList>, boost::noncopyable >
                     ("NeighborListTree", init< boost::shared_ptr<SystemDefinition>, Scalar, Scalar >())
                     ;
    }
