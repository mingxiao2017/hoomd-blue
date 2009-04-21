#ifdef WIN32
#pragma warning( push )
#pragma warning( disable : 4244 )
#endif

#include "HarmonicDihedralForceComputeGPU.h"

#include <boost/python.hpp>
using namespace boost::python;

#include <boost/bind.hpp>
using namespace boost;

using namespace std;

/*! \param pdata ParticleData to compute dihedral forces on
*/
HarmonicDihedralForceComputeGPU::HarmonicDihedralForceComputeGPU(boost::shared_ptr<ParticleData> pdata)
	: HarmonicDihedralForceCompute(pdata)
	{
	// can't run on the GPU if there aren't any GPUs in the execution configuration
	if (exec_conf.gpu.size() == 0)
		{
		cerr << endl << "***Error! Creating a DihedralForceComputeGPU with no GPU in the execution configuration" << endl << endl;
		throw std::runtime_error("Error initializing DihedralForceComputeGPU");
		}
		
	// default block size is the highest performance in testing on different hardware
	// choose based on compute capability of the device
	cudaDeviceProp deviceProp;
	int dev;
	exec_conf.gpu[0]->call(bind(cudaGetDevice, &dev));
	exec_conf.gpu[0]->call(bind(cudaGetDeviceProperties, &deviceProp, dev));
	if (deviceProp.major == 1 && deviceProp.minor == 0)
		m_block_size = 64;
	else if (deviceProp.major == 1 && deviceProp.minor == 1)
		m_block_size = 64;
	else if (deviceProp.major == 1 && deviceProp.minor < 4)
		m_block_size = 288;
	else
		{
		cout << "***Warning! Unknown compute " << deviceProp.major << "." << deviceProp.minor << " when tuning block size for HarmonicDihedralForceComputeGPU" << endl;
		m_block_size = 64;
		}
	
	// allocate and zero device memory
	m_gpu_params.resize(exec_conf.gpu.size());
	exec_conf.tagAll(__FILE__, __LINE__);
	for (unsigned int cur_gpu = 0; cur_gpu < exec_conf.gpu.size(); cur_gpu++)
		{
		exec_conf.gpu[cur_gpu]->call(bind(cudaMalloc, (void**)((void*)&m_gpu_params[cur_gpu]), m_dihedral_data->getNDihedralTypes()*sizeof(float4)));
		exec_conf.gpu[cur_gpu]->call(bind(cudaMemset, (void*)m_gpu_params[cur_gpu], 0, m_dihedral_data->getNDihedralTypes()*sizeof(float4)));
		}
	
	m_host_params = new float4[m_dihedral_data->getNDihedralTypes()];
	memset(m_host_params, 0, m_dihedral_data->getNDihedralTypes()*sizeof(float4));
	}
	
HarmonicDihedralForceComputeGPU::~HarmonicDihedralForceComputeGPU()
	{
	// free memory on the GPU
	exec_conf.tagAll(__FILE__, __LINE__);
	for (unsigned int cur_gpu = 0; cur_gpu < exec_conf.gpu.size(); cur_gpu++)
		{	
		exec_conf.gpu[cur_gpu]->call(bind(cudaFree, (void*)m_gpu_params[cur_gpu]));
		m_gpu_params[cur_gpu] = NULL;
		}
	
	// free memory on the CPU
	delete[] m_host_params;
	m_host_params = NULL;
	}

/*! \param type Type of the dihedral to set parameters for
	\param K Stiffness parameter for the force computation
	\param sign the sign of the cosine term
        \param multiplicity the multiplicity of the cosine term 
	
	Sets parameters for the potential of a particular dihedral type and updates the 
	parameters on the GPU.
*/
void HarmonicDihedralForceComputeGPU::setParams(unsigned int type, Scalar K, int sign, unsigned int multiplicity)
	{
	HarmonicDihedralForceCompute::setParams(type, K, sign, multiplicity);
	
	// update the local copy of the memory
	m_host_params[type] = make_float4(float(K), float(sign), float(multiplicity), 0.0f);
	
	// copy the parameters to the GPU
	exec_conf.tagAll(__FILE__, __LINE__);
	for (unsigned int cur_gpu = 0; cur_gpu < exec_conf.gpu.size(); cur_gpu++)
		exec_conf.gpu[cur_gpu]->call(bind(cudaMemcpy, m_gpu_params[cur_gpu], m_host_params, m_dihedral_data->getNDihedralTypes()*sizeof(float4), cudaMemcpyHostToDevice));
	}

/*! Internal method for computing the forces on the GPU. 
	\post The force data on the GPU is written with the calculated forces
	
	\param timestep Current time step of the simulation
	
	Calls gpu_compute_harmonic_dihedral_forces to do the dirty work.
*/
void HarmonicDihedralForceComputeGPU::computeForces(unsigned int timestep)
	{
	// start the profile
	if (m_prof) m_prof->push(exec_conf, "Dihedral");
		
	vector<gpu_dihedraltable_array>& gpu_dihedraltable = m_dihedral_data->acquireGPU();
	
	// the dihedral table is up to date: we are good to go. Call the kernel
	vector<gpu_pdata_arrays>& pdata = m_pdata->acquireReadOnlyGPU();
	gpu_boxsize box = m_pdata->getBoxGPU();
	
	// run the kernel in parallel on all GPUs
	exec_conf.tagAll(__FILE__, __LINE__);
	for (unsigned int cur_gpu = 0; cur_gpu < exec_conf.gpu.size(); cur_gpu++)
		exec_conf.gpu[cur_gpu]->callAsync(bind(gpu_compute_harmonic_dihedral_forces, m_gpu_forces[cur_gpu].d_data, pdata[cur_gpu], box, gpu_dihedraltable[cur_gpu], m_gpu_params[cur_gpu], m_dihedral_data->getNDihedralTypes(), m_block_size));
	exec_conf.syncAll();
		
	// the force data is now only up to date on the gpu
	m_data_location = gpu;
	
	m_pdata->release();
	
        // UNCOMMENT BELOW FOR SOME KIND OF PERFORMANCE CHECK... but first, count all the flops + memory transfers
	//int64_t mem_transfer = m_pdata->getN() * 4+16+20 + m_dihedral_data->getNumDihedral() * 2 * (8+16+8);
	//int64_t flops = m_dihedral_data->getNumDihedral() * 2 * (3+12+16+3+7);
	//if (m_prof)	m_prof->pop(exec_conf, flops, mem_transfer);
	}

void export_HarmonicDihedralForceComputeGPU()
	{
	class_<HarmonicDihedralForceComputeGPU, boost::shared_ptr<HarmonicDihedralForceComputeGPU>, bases<HarmonicDihedralForceCompute>, boost::noncopyable >
		("HarmonicDihedralForceComputeGPU", init< boost::shared_ptr<ParticleData> >())
		.def("setBlockSize", &HarmonicDihedralForceComputeGPU::setBlockSize)
		;
	}
