#ifndef COLLISION_BGK_S_HPP_INCLUDED
#define COLLISION_BGK_S_HPP_INCLUDED

/**
 * \file     collision_bgk-s.hpp
 * \mainpage BGK collision operator with Smagorinsky turbulence model
*/

#include <algorithm>
#include <cmath>
#if __has_include (<omp.h>)
    #include <omp.h>
#endif

#include "../../continuum/continuum.hpp"
#include "../../general/memory_alignment.hpp"
#include "../population.hpp"
#include "collision.hpp"


/**\class  BGK_Smagorinsky
 * \brief  BGK collision operator for arbitrary lattice
 * \note   "A Lattice Boltzmann Subgrid Model for High Reynolds Number Flows"
 *         S. Hou, J. Sterling, S. Chen, G.D. Doolen
 *         (1994)
 *         arXiv: arXiv:comp-gas/9401004
 * 
 * \tparam NX   Simulation domain resolution in x-direction
 * \tparam NY   Simulation domain resolution in y-direction
 * \tparam NZ   Simulation domain resolution in z-direction
 * \tparam LT   Static lattice::DdQq class containing discretisation parameters
 * \tparam T    Floating data type used for simulation
*/
template <unsigned int NX, unsigned int NY, unsigned int NZ, class LT, typename T>
class BGK_Smagorinsky: public CollisionOperator<NX,NY,NZ,LT,T,BGK_Smagorinsky<NX,NY,NZ,LT,T>>
{
    using CO = CollisionOperator<NX,NY,NZ,LT,T,BGK_Smagorinsky<NX,NY,NZ,LT,T>>; 

    public:
        /**\brief     Constructor
         * 
         * \param[in] population   Population object holding microscopic distributions
         * \param[in] continuum    Continuum object holding macroscopic variables
         * \param[in] Re           The Reynolds number
         * \param[in] U            The macroscopic velocity
         * \param[in] L            The characteristic length
         * \param[in] p            Index of relevant population
        */
        BGK_Smagorinsky(std::shared_ptr<Population<NX,NY,NZ,LT>> population, std::shared_ptr<Continuum<NX,NY,NZ,T>> continuum, 
                        T const Re, T const U, unsigned int const L, unsigned int const p = 0):
            CO(population, continuum, p), population_(population), continuum_(continuum), p_(p),
            nu_(U*static_cast<T>(L) / Re), 
            tau_(nu_/(LT::CS*LT::CS) + 1.0/ 2.0), omega_(1.0/tau_)
        {
            return;
        }

        /**\fn        implementation
         * \brief     Implementation of the BGK collision operator with Smagorinsky turbulence model
         * 
         * \tparam    AA       The timestep in the AA-pattern
         * \param[in] isSave   Boolean parameter whether the macroscopic values should be saved or not
        */
        template<timestep AA>
        void implementation(bool const isSave);

    protected:
        std::shared_ptr<Population<NX,NY,NZ,LT>> population_;
        std::shared_ptr<Continuum<NX,NY,NZ,T>>   continuum_;
        unsigned int const p_;

        T const nu_;
        T const tau_;
        T const omega_;

        static constexpr T CS = 0.17; ///< Smagorinsky constant
};


template <unsigned int NX, unsigned int NY, unsigned int NZ, class LT, typename T> template<timestep AA>
void BGK_Smagorinsky<NX,NY,NZ,LT,T>::implementation(bool const isSave)
{
    #pragma omp parallel for default(none) shared(continuum_,population_) firstprivate(isSave,p_) schedule(static,1)
    for(unsigned int block = 0; block < CO::NUM_BLOCKS_; ++block)
    {
        unsigned int const z_start = CO::BLOCK_SIZE_ * (block / (CO::NUM_BLOCKS_X_*CO::NUM_BLOCKS_Y_));
        unsigned int const   z_end = std::min(z_start + CO::BLOCK_SIZE_, NZ);

        for(unsigned int z = z_start; z < z_end; ++z)
        {
            unsigned int const z_n[3] = { (NZ + z - 1) % NZ, z, (z + 1) % NZ };

            unsigned int const y_start = CO::BLOCK_SIZE_*((block % (CO::NUM_BLOCKS_X_*CO::NUM_BLOCKS_Y_)) / CO::NUM_BLOCKS_X_);
            unsigned int const   y_end = std::min(y_start + CO::BLOCK_SIZE_, NY);

            for(unsigned int y = y_start; y < y_end; ++y)
            {
                unsigned int const y_n[3] = { (NY + y - 1) % NY, y, (y + 1) % NY };

                unsigned int const x_start = CO::BLOCK_SIZE_*(block % CO::NUM_BLOCKS_X_);
                unsigned int const   x_end = std::min(x_start + CO::BLOCK_SIZE_, NX);

                for(unsigned int x = x_start; x < x_end; ++x)
                {
                    unsigned int const x_n[3] = { (NX + x - 1) % NX, x, (x + 1) % NX };

                    /// load distributions
                    alignas(CACHE_LINE) T f[LT::ND] = {0.0};

                    #pragma GCC unroll (2)
                    for(unsigned int n = 0; n <= 1; ++n)
                    {
                        #pragma GCC unroll (16)
                        for(unsigned int d = n; d < LT::HSPEED; ++d)
                        {
                            f[n*LT::OFF + d] = population_->F_[population_-> template AA_IndexRead<AA>(x_n,y_n,z_n,n,d,p_)];
                        }
                    }

                    /// macroscopic values
                    T rho = 0.0;
                    T u   = 0.0;
                    T v   = 0.0;
                    T w   = 0.0;
                    #pragma GCC unroll (2)
                    for(unsigned int n = 0; n <= 1; ++n)
                    {
                        #pragma GCC unroll (16)
                        for(unsigned int d = n; d < LT::HSPEED; ++d)
                        {
                            unsigned int const curr = n*LT::OFF + d;
                            rho += f[curr];
                            u   += f[curr]*LT::DX[curr];
                            v   += f[curr]*LT::DY[curr];
                            w   += f[curr]*LT::DZ[curr];
                        }
                    }
                    u /= rho;
                    v /= rho;
                    w /= rho;

                    if (isSave == true)
                    {
                        continuum_->operator()(x, y, z, 0) = rho;
                        continuum_->operator()(x, y, z, 1) = u;
                        continuum_->operator()(x, y, z, 2) = v;
                        continuum_->operator()(x, y, z, 3) = w;
                    }

                    /// equilibrium distributions and non-equilibrium part
                    alignas(CACHE_LINE) T feq[LT::ND]  = {0.0};
                    alignas(CACHE_LINE) T fneq[LT::ND] = {0.0};

                    T const uu = - 1.0/(2.0*LT::CS*LT::CS)*(u*u + v*v + w*w);

                    #pragma GCC unroll (2)
                    for(unsigned int n = 0; n <= 1; ++n)
                    {
                        #pragma GCC unroll (16)
                        for(unsigned int d = n; d < LT::HSPEED; ++d)
                        {
                            unsigned int const curr = n*LT::OFF + d;
                            T const cu = 1.0/(LT::CS*LT::CS)*(u*LT::DX[curr] + v*LT::DY[curr] + w*LT::DZ[curr]);
                            feq[curr]  = LT::W[curr]*(rho + rho*(cu*(1.0 + 0.5*cu) + uu));
                            fneq[curr] = f[curr] - feq[curr];
                        }
                    }

                    /// strain-rate tensor
                    T p_xx = 0.0;
                    T p_yy = 0.0;
                    T p_zz = 0.0;
                    T p_xy = 0.0;
                    T p_xz = 0.0;
                    T p_yz = 0.0;
                    #pragma GCC unroll (2)
                    for(unsigned int n = 0; n <= 1; ++n)
                    {
                        #pragma GCC unroll (16)
                        for(unsigned int d = n; d < LT::HSPEED; ++d)
                        {
                            unsigned int const curr = n*LT::OFF + d;
                            p_xx += LT::DX[curr]*LT::DX[curr]*fneq[curr];
                            p_yy += LT::DY[curr]*LT::DY[curr]*fneq[curr];
                            p_zz += LT::DZ[curr]*LT::DZ[curr]*fneq[curr];

                            p_xy += LT::DX[curr]*LT::DY[curr]*fneq[curr];
                            p_xz += LT::DX[curr]*LT::DZ[curr]*fneq[curr];
                            p_yz += LT::DY[curr]*LT::DZ[curr]*fneq[curr];
                        }
                    }

                    // calculate overall momentum flux
                    T const p_ij = std::sqrt(p_xx*p_xx + p_yy*p_yy + p_zz*p_zz + 2*p_xy*p_xy + 2*p_xz*p_xz + 2*p_yz*p_yz);

                    // calculate turbulent relaxation
                    T const tau_t = 0.5*(std::sqrt(tau_*tau_ + 2*sqrt(2)*CS*CS*p_ij/(rho*LT::CS*LT::CS*LT::CS*LT::CS)) - tau_);
                    T const omega = 1.0/(tau_ + tau_t);

                    /// collision and streaming
                    #pragma GCC unroll (2)
                    for(unsigned int n = 0; n <= 1; ++n)
                    {
                        #pragma GCC unroll (16)
                        for(unsigned int d = n; d < LT::HSPEED; ++d)
                        {
                            unsigned int const curr = n*LT::OFF + d;
                            population_->F_[population_-> template AA_IndexWrite<AA>(x_n,y_n,z_n,n,d,p_)] = f[curr] + omega*(feq[curr] - f[curr]);
                        }
                    }
                }
            }
        }
    }

    return;
}

#endif //COLLISION_BGK_S_HPP_INCLUDED
