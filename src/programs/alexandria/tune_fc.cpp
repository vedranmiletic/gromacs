/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2016, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \brief
 * Implements part of the alexandria program.
 * \author David van der Spoel <david.vanderspoel@icm.uu.se>
 */
#include "gmxpre.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "gromacs/commandline/pargs.h"
#include "gromacs/commandline/viewit.h"
#include "gromacs/fileio/confio.h"
#include "gromacs/fileio/xvgr.h"
#include "gromacs/gmxlib/network.h"
#include "gromacs/gmxlib/nrnb.h"
#include "gromacs/gmxpreprocess/convparm.h"
#include "gromacs/gmxpreprocess/gpp_atomtype.h"
#include "gromacs/gmxpreprocess/grompp.h"
#include "gromacs/gmxpreprocess/pdb2top.h"
#include "gromacs/linearalgebra/matrix.h"
#include "gromacs/listed-forces/bonded.h"
#include "gromacs/math/units.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdlib/force.h"
#include "gromacs/mdlib/mdatoms.h"
#include "gromacs/mdlib/shellfc.h"
#include "gromacs/mdlib/vsite.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/random/random.h"
#include "gromacs/statistics/statistics.h"
#include "gromacs/timing/wallcycle.h"
#include "gromacs/topology/atomprop.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/topology/topology.h"
#include "gromacs/utility/coolstuff.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/init.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/stringutil.h"

#include "nmsimplex.h"

// alexandria stuff
#include "gentop_core.h"
#include "gmx_simple_comm.h"
#include "moldip.h"
#include "molprop.h"
#include "molprop_util.h"
#include "molprop_xml.h"
#include "molselect.h"
#include "mymol.h"
#include "poldata.h"
#include "poldata_xml.h"
#include "qgen_eem.h"
#include "stringutil.h"

/*! \brief Write a csv file containing molecule names and bond energy
 *
 * Writes the whole bond energy matrix.
 */
static void dump_csv(const std::vector<std::string>        &ctest,
                     const std::vector<alexandria::MyMol>  &mm,
                     const std::vector<int>                &ntest,
                     const std::vector<double>             &Edissoc,
                     double                               **a,
                     double                                *x)
{
    FILE *csv = gmx_ffopen("tune_fc.csv", "w");
    fprintf(csv, ",");
    for (auto j : ctest)
    {
        fprintf(csv, "%s,", j.c_str());
    }
    fprintf(csv, "\n");
    int i = 0;
    for (auto &mymol : mm)
    {
        fprintf(csv, "%s,", mymol.molProp()->getMolname().c_str());
        for (size_t j = 0; (j < ctest.size()); j++)
        {
            fprintf(csv, "%g,", a[j][i]);
        }
        fprintf(csv, "%.3f\n", x[i]);
        i++;
    }
    fprintf(csv, "Total,");
    for (auto j : ntest)
    {
        fprintf(csv, "%d,", j);
    }
    fprintf(csv, "\n");
    fprintf(csv, "Edissoc,");
    for (auto j : Edissoc)
    {
        fprintf(csv, "%.3f,", j);
    }
    fprintf(csv, "\n");
    fclose(csv);
}

namespace alexandria
{

/*! \brief Helper class storing bond/angle/dihedral names
 *
 * For one bond/angle/dihedral here the name of the bondtypes
 * are stored as in e.g. c c h for an angle, along with the number
 * of occurrences in the force field.
 */
class BondNames
{
    private:
        //! Number of copies in the molecule data set
        int                 ncopies_;
        //! Name of this bond/angle/dihedral
        std::string         name_;
        //! String holding all the parameters
        std::string         params_;
        //! Vector containing all the parameters
        std::vector<double> p_;
        //! The bond order in case this is a bond
        double              bondorder_;
        //! Index in Poldata structure
        int                 poldataIndex_;
        //! Internal routine to extract the parameters
        void extractParams();
    public:
        BondNames() {};

        BondNames(int                ncopies,
                  const std::string &name,
                  const std::string &params,
                  int                index,
                  double             bondorder = 0) :
            ncopies_(ncopies), name_(name), params_(params), bondorder_(bondorder), poldataIndex_(index)
        {
            extractParams();
        }

        void inc() { ncopies_++; }

        int nCopies() const { return ncopies_; }

        void setParamString(const std::string &params);

        const std::string &name() const { return name_; }

        double bondorder() const { return bondorder_; }

        int poldataIndex() const { return poldataIndex_; }

        const std::string &paramString() const { return params_; }

        const std::vector<double> &paramValues() const { return p_; }

        size_t nParams() const { return p_.size(); }
};
typedef std::vector<BondNames>::iterator BondNamesIterator;

void BondNames::setParamString(const std::string &params)
{
    params_ = params;
    extractParams();
}

void BondNames::extractParams()
{
    std::vector<std::string> p = gmx::splitString(params_);
    p_.clear();
    for (const auto &d : p)
    {
        p_.push_back(atof(d.c_str()));
    }
}

/*! \brief Class holding for one type of interactions all names
 *
 * Class holding the OptNames for one interaction type.
 */
class ForceConstants
{
    private:
        int                    bt_;
        int                    ft_;
        int                    nbad_;
        bool                   bOpt_;
        std::vector<BondNames> bn_;
        //opt_bad_t *_bad;
        std::vector<int>       reverseIndex_;
        std::vector<double>    params_;
    public:
        ForceConstants(int bt, bool bOpt) : bt_(bt), bOpt_(bOpt)
        {
            ft_   = 0;
            nbad_ = 0;
        }

        void addForceConstant(BondNames bn) { bn_.push_back(bn); }

        void analyzePoldata(const Poldata &pd);

        void analyzeIdef(std::vector<MyMol> &mm,
                         const Poldata      &pd);

        /*! \brief Make reverse index from Poldata to BondNames
         *
         * The BondNames structure stores the Poldata index for
         * all interactions. This routine makes an index to convert
         * the Poldata index to the index in BondNames.
         */
        void makeReverseIndex();

        int reverseIndex(int poldataIndex) 
        { 
            GMX_RELEASE_ASSERT(poldataIndex >= 0 && poldataIndex < static_cast<int>(reverseIndex_.size()), "Incorrect poldataIndex"); 
            return reverseIndex_[poldataIndex]; 
        }

        int bt() const { return bt_; }

        int ft() const { return ft_; }

        void dump(FILE *fp) const;

        BondNamesIterator beginBN() { return bn_.begin(); }

        BondNamesIterator endBN() { return bn_.end(); }

        int nbad() const { return nbad_; }
};

void ForceConstants::analyzePoldata(const Poldata &pd)
{
    if (!bOpt_)
    {
        return;
    }
    switch (bt_)
    {
        case ebtsBONDS:
            nbad_ = pd.getNgtBond();
            ft_   = pd.getBondFtype();
            break;
        case ebtsANGLES:
            nbad_ = pd.getNgtAngle();
            ft_   = pd.getAngleFtype();
            break;
        case ebtsPDIHS:
            nbad_ = pd.getNgtDihedral(egdPDIHS);
            ft_   = pd.getDihedralFtype(egdPDIHS);
            break;
        case ebtsIDIHS:
            nbad_ = pd.getNgtDihedral(egdIDIHS);
            ft_   = pd.getDihedralFtype(egdIDIHS);
            break;
        default:
            gmx_fatal(FARGS, "Boe");
    }
}

void ForceConstants::analyzeIdef(std::vector<MyMol> &mm,
                                 const Poldata      &pd)
{
    std::string  aai, aaj, aak, aal;

    if (!bOpt_)
    {
        return;
    }
    for (auto &mymol : mm)
    {
        for (int i = 0; (i < mymol.ltop_->idef.il[ft_].nr); i += interaction_function[ft_].nratoms+1)
        {
            std::string params;
            bool        found     = false;
            double      bondorder = 0;
            int         ai        = mymol.ltop_->idef.il[ft_].iatoms[i+1];
            int         aj        = mymol.ltop_->idef.il[ft_].iatoms[i+2];
            if (pd.atypeToBtype( *mymol.topology_->atoms.atomtype[ai], aai) &&
                pd.atypeToBtype( *mymol.topology_->atoms.atomtype[aj], aaj))
            {
                int  index = 0;
                char buf[STRLEN];
                switch (bt_)
                {
                    case ebtsBONDS:
                    {
                        GtBondConstIterator gtb = pd.findBond(aai, aaj, 0);
                        if (pd.getBondEnd() != gtb)
                        {
                            sprintf(buf, "%s %s",
                                    gtb->getAtom1().c_str(),
                                    gtb->getAtom2().c_str());
                            params    = gtb->getParams();
                            bondorder = gtb->getBondorder();
                            index     = gtb - pd.getBondBegin();
                            found     = true;
                        }
                    }
                    break;
                    case ebtsANGLES:
                    {
                        int ak  = mymol.ltop_->idef.il[ft_].iatoms[i+3];
                        if (pd.atypeToBtype( *mymol.topology_->atoms.atomtype[ak], aak))
                        {
                            GtAngleConstIterator gta = pd.findAngle(aai, aaj, aak);
                            if (pd.getAngleEnd() != gta)
                            {
                                sprintf(buf, "%s %s %s", gta->getAtom1().c_str(),
                                        gta->getAtom2().c_str(), gta->getAtom3().c_str());
                                params = gta->getParams();
                                index  = gta - pd.getAngleBegin();
                                found  = true;
                            }
                        }
                    }
                    break;
                    case ebtsPDIHS:
                    case ebtsIDIHS:
                    {
                        int ak  = mymol.ltop_->idef.il[ft_].iatoms[i+3];
                        int al  = mymol.ltop_->idef.il[ft_].iatoms[i+4];
                        if (pd.atypeToBtype( *mymol.topology_->atoms.atomtype[ak], aak) &&
                            pd.atypeToBtype( *mymol.topology_->atoms.atomtype[al], aal))
                        {
                            int                   egd = (bt_ == ebtsPDIHS) ? egdPDIHS : egdIDIHS;
                            DihedralConstIterator gtd = pd.findDihedral(egd, aai, aaj, aak, aal);
                            if (pd.getDihedralEnd(egd) != gtd)
                            {
                                sprintf(buf, "%s %s %s %s",
                                        gtd->getAtom1().c_str(), gtd->getAtom2().c_str(),
                                        gtd->getAtom3().c_str(), gtd->getAtom4().c_str());
                                params = gtd->getParams();
                                index  = gtd - pd.getDihedralBegin(egd);
                                found  = true;
                            }
                        }
                    }
                    break;
                }
                if (found)
                {
                    auto c = std::find_if(bn_.begin(), bn_.end(),
                                          [buf](BondNames &bn)
                                          { return bn.name().compare(buf); });
                    if (c != bn_.end())
                    {
                        c->inc();
                    }
                    else
                    {
                        BondNames bn(1, buf, params, bondorder, index);
                        addForceConstant(bn);
                    }
                }
            }
        }
    }
}

void ForceConstants::makeReverseIndex()
{
    int j = 0;
    reverseIndex_.resize(nbad(), 0);
    for (const auto &i : bn_)
    {
        reverseIndex_[i.poldataIndex()] = j++;
    }
}

void ForceConstants::dump(FILE *fp) const
{
    const char  *btsnames[ebtsNR] =
    { "bond", "angle", "proper", "improper", NULL, NULL };

    if (bOpt_)
    {
        int ntot = 0;
        fprintf(fp, "Interaction  Bondtypes             Copies Poldata entry\n");
        for (const auto &i : bn_)
        {
            fprintf(fp, "%-10s  %-20s  %5d  %5d\n",
                    btsnames[bt_], i.name().c_str(), i.nCopies(), i.poldataIndex());
            ntot += i.nCopies();
        }
        fprintf(fp, "%-8s %d of %4d types\n", btsnames[bt_], ntot,
                static_cast<int>(bn_.size()));
    }
}

class OptParam : public MolDip
{
    private:
        std::vector<ForceConstants> ForceConstants_;
        std::vector<double>         param_, orig_, best_, lower_, upper_, psigma_;

    public:
        OptParam() {};

        ~OptParam() {};

        void InitOpt(FILE *fplog,
                     bool bOpt[ebtsNR],
                     real factor);

        /*! \brief Check whether molecules are supported by the force field
         *
         * Check whether all the atomtypes in the molecules are present
         * in the force field file. If not the molecules are removed.
         *
         * Routine also divides molecules over processors.
         */
        void checkSupport(FILE *fp,
                          bool  bOpt[]);
        /*! \brief Compute the dissociation energies for all the bonds
         *
         * Given all the bonds and the enthalpies of formation of all
         * molecules, we can approximate the dissociation enthalpy (D0 in the
         * Morse potential by least squares fitting the D0 to reproduce the
         * molecular energy (Delta H formation of molecule - Delta H formation of
         * the atoms). This is a crude approximation since all other energy
         * terms in the force field are ignored, however the dissociation
         * energy is the largest contribution to the molecular energy.
         */
        void getDissociationEnergy(FILE *fplog);

        //! Fill parameter list from ForceConstants
        void Opt2List();

        //! Copy optimized parameters back to Poldata
        void List2Opt();
        void Print(FILE *fp);
        double CalcDeviation();
        double EnergyFunction(double v[]);
        void GuessAll(int iter, real stepsize,
                      bool bRandom, gmx_rng_t rng);
        void Optimize(FILE *fp, FILE *fplog,
                      int maxiter, //real tol,
                      int nrun, real stepsize, int seed,
                      bool bRandom, const gmx_output_env_t *oenv,
                      int nprint,
                      const char *xvgconv, const char *xvgepot,
                      real temperature);
        void Bayes(FILE *fplog, const char *xvgconv, const char *xvgepot,
                   int nprint,
                   double step,
                   unsigned int seed,
                   real temperature,
                   int    maxiter,
                   double *chi2,
                   const gmx_output_env_t *oenv);
        void PrintSpecs(FILE *fp, char *title,
                        const char *xvg, const gmx_output_env_t *oenv,
                        bool bCheckOutliers);
};

void OptParam::Opt2List()
{
    param_.clear();
    for (auto &fc : ForceConstants_)
    {
        for (BondNamesIterator b = fc.beginBN(); b  < fc.endBN(); ++b)
        {
            for (const auto &p : b->paramValues())
            {
                param_.push_back(p);
            }
        }
    }
}

void OptParam::List2Opt()
{
    int n = 0;
    for (auto &fc : ForceConstants_)
    {
        for (BondNamesIterator b = fc.beginBN(); b  < fc.endBN(); ++b)
        {
            char buf[STRLEN];
            buf[0] = '\0';

            for (size_t p = 0; (p < b->nParams()); p++)
            {
                strncat(buf, " ", sizeof(buf)-1);
                strncat(buf, gmx_ftoa(param_[n++]).c_str(), sizeof(buf)-1);
            }
            b->setParamString(buf);
            const std::vector<std::string> bondtypes =
                gmx::splitString(b->name());

            switch (fc.bt())
            {
                case ebtsBONDS:
                    pd_.setBondParams(bondtypes[0], bondtypes[1],
                                      0, 0, 0, b->bondorder(), buf);
                    break;
                case ebtsANGLES:
                    pd_.setAngleParams(bondtypes[0], bondtypes[1], bondtypes[2],
                                       0, 0, 0, buf);
                    break;
                case ebtsPDIHS:
                    pd_.setDihedralParams(egdPDIHS, bondtypes[0], bondtypes[1],
                                          bondtypes[2], bondtypes[3], 0, 0, 0, buf);
                    break;
                case ebtsIDIHS:
                    pd_.setDihedralParams(egdIDIHS, bondtypes[0], bondtypes[1],
                                          bondtypes[2], bondtypes[3], 0, 0, 0, buf);
                    break;
                default:
                    gmx_fatal(FARGS, "Unsupported bts %d", fc.bt());
            }
        }
    }
}

void OptParam::checkSupport(FILE *fp,
                            bool  bOpt[])
{
    int ntotal  = _mymol.size();
    int nlocal  = 0;

    for (auto mymol = _mymol.begin(); mymol < _mymol.end(); )
    {
        if (mymol->eSupp != eSupportLocal)
        {
            continue;
        }
        bool bSupport = true;

        for (int bt = 0; bSupport && (bt <= ebtsIDIHS); bt++)
        {
            int  ft;
            if (bOpt[bt])
            {
                switch (bt)
                {
                    case ebtsBONDS:
                        ft = pd_.getBondFtype();
                        break;
                    case ebtsANGLES:
                        ft = pd_.getAngleFtype();
                        break;
                    case ebtsPDIHS:
                        ft = pd_.getDihedralFtype(egdPDIHS);
                        break;
                    case ebtsIDIHS:
                        ft = pd_.getDihedralFtype( egdIDIHS);
                        break;
                    default:
                        gmx_fatal(FARGS, "Boe");
                }
                bSupport = (mymol->ltop_ != nullptr);
                for (int i = 0; bSupport && (i < mymol->ltop_->idef.il[ft].nr); i += interaction_function[ft].nratoms+1)
                {
                    int         ai, aj, ak, al;
                    std::string aai, aaj, aak, aal;

                    ai  = mymol->ltop_->idef.il[ft].iatoms[i+1];
                    aj  = mymol->ltop_->idef.il[ft].iatoms[i+2];
                    if (!(pd_.atypeToBtype(*mymol->topology_->atoms.atomtype[ai], aai) &&
                          pd_.atypeToBtype(*mymol->topology_->atoms.atomtype[aj], aaj)))
                    {
                        bSupport = false;
                    }
                    switch (bt)
                    {
                        case ebtsBONDS:
                        {
                            if (pd_.findBond(aai, aaj, 0) == pd_.getBondEnd())
                            {
                                bSupport = false;
                                if (debug)
                                {
                                    fprintf(debug, "Cannot find bond %s-%s\n", aai.c_str(), aaj.c_str());
                                }
                            }
                            break;
                        }
                        case ebtsANGLES:
                        {
                            ak  = mymol->ltop_->idef.il[ft].iatoms[i+3];
                            if (!pd_.atypeToBtype( *mymol->topology_->atoms.atomtype[ak], aak))
                            {
                                bSupport = false;
                            }
                            else
                            {
                                if (pd_.findAngle(aai, aaj, aak) == pd_.getAngleEnd())
                                {
                                    bSupport = false;
                                    if (debug)
                                    {
                                        fprintf(debug, "Cannot find angle %s-%s-%s\n", 
                                                aai.c_str(), aaj.c_str(), aak.c_str());
                                    }
                                }
                            }
                        }
                        break;
                        case ebtsPDIHS:
                        case ebtsIDIHS:
                        {
                            ak  = mymol->ltop_->idef.il[ft].iatoms[i+3];
                            al  = mymol->ltop_->idef.il[ft].iatoms[i+4];
                            if (!(pd_.atypeToBtype( *mymol->topology_->atoms.atomtype[ak], aak) &&
                                  pd_.atypeToBtype( *mymol->topology_->atoms.atomtype[al], aal)))
                            {
                                bSupport = false;
                            }
                            else
                            {
                                int egd = (bt == ebtsPDIHS) ? egdPDIHS : egdIDIHS;
                                if (pd_.findDihedral(egd, aai, aaj, aak, aal) == pd_.getDihedralEnd(egd))
                                {
                                    bSupport = false;
                                    if (debug)
                                    {
                                        fprintf(debug, "Cannot find dihedral %s-%s-%s-%s\n", 
                                                aai.c_str(), aaj.c_str(), aak.c_str(), aal.c_str());
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (!bSupport)
        {
            fprintf(stderr, "No force field support for %s\n",
                    mymol->molProp()->getMolname().c_str());
            mymol = _mymol.erase(mymol);
        }
        else
        {
            mymol++;
            nlocal++;
        }
    }
    if (PAR(_cr))
    {
        gmx_sumi(1, &nlocal, _cr);
    }
    if (NULL != fp)
    {
        fprintf(fp, "%d out of %d molecules have support in the force field.\n",
                nlocal, ntotal);
    }
}

void OptParam::getDissociationEnergy(FILE *fplog)
{
    double                    **a;
    std::vector<double>         rhs;
    std::vector<int>            ntest;
    std::vector<std::string>    ctest;

    int nD   = ForceConstants_[ebtsBONDS].nbad();
    int nMol = _mymol.size();
    if ((0 == nD) || (0 == nMol))
    {
        gmx_fatal(FARGS, "Number of variables is %d and number of molecules is %d",
                  nD, nMol);
    }
    a = alloc_matrix(nD, nMol);
    ntest.resize(nD, 0);
    ctest.resize(nD);

    fprintf(fplog, "There are %d different bondtypes to optimize the heat of formation\n",
            nD);
    fprintf(fplog, "There are %d (experimental) reference heat of formation.\n", nMol);

    int ftb = 0; //pd_.getBondFtype();
    int j   = 0;
    for (std::vector<alexandria::MyMol>::iterator mymol = _mymol.begin();
         (mymol < _mymol.end()); mymol++, j++)
    {
        for (int i = 0; (i < mymol->ltop_->idef.il[ftb].nr); i += interaction_function[ftb].nratoms+1)
        {
            int ai = mymol->ltop_->idef.il[ftb].iatoms[i+1];
            int aj = mymol->ltop_->idef.il[ftb].iatoms[i+2];
            std::string aai, aaj;
            if (pd_.atypeToBtype(*mymol->topology_->atoms.atomtype[ai], aai) &&
                pd_.atypeToBtype(*mymol->topology_->atoms.atomtype[aj], aaj))
            {
                GtBondIterator gtb = pd_.findBond(aai, aaj, 0);
                if (gtb != pd_.getBondEnd())
                {
                    int gt  = gtb - pd_.getBondBegin();
                    //int gti = ForceConstants_[ebtsBONDS].reverseIndex(gt);

                    a[gt][j]++;
                    ntest[gt]++;
                    if (ctest[gt].empty())
                    {
                        char buf[STRLEN];
                        snprintf(buf, sizeof(buf), "%s-%s", aai.c_str(), aaj.c_str());
                        ctest[gt].assign(buf);
                    }
                }
            }
            else
            {
                gmx_fatal(FARGS, "No parameters for bond %s-%s in the force field, atoms %s-%s mol %s",
                          aai.c_str(), aaj.c_str(),
                          *mymol->topology_->atoms.atomtype[ai],
                          *mymol->topology_->atoms.atomtype[aj],
                          mymol->molProp()->getIupac().c_str());
            }
        }
        rhs.push_back(mymol->Emol);
    }
    char buf[STRLEN];
    snprintf(buf, sizeof(buf), "Inconsistency in number of energies nMol %d != #rhs %d", nMol, static_cast<int>(rhs.size()));
    GMX_RELEASE_ASSERT(static_cast<int>(rhs.size()) == nMol, buf);

    int nzero = std::count_if(ntest.begin(), ntest.end(), 
                              [](const int n) { return n == 0; });
    printf("There are %d bondtypes without support\n", nzero);
    double **a2 = alloc_matrix(nD-nzero, nMol);
    int i2 = 0;
    for(int i = 0; i<nD; i++)
    {
        if (ntest[i] > 0)
        {
            for(int j = 0; j<nMol; j++)
            {
                a2[i2][j] = a[i][j];
            }
            ntest[i2] = ntest[i];
            ctest[i2] = ctest[i];
            i2++;
        }
    }
    std::vector<double> Edissoc(i2);
    ctest.resize(i2);
    ntest.resize(i2);
    dump_csv(ctest, _mymol, ntest, Edissoc, a, rhs.data());
    double chi2 = multi_regression(debug, nMol, rhs.data(), i2, a2, Edissoc.data());
    
    for (size_t i = 0; (i < ctest.size()); i++)
    {
        if (fplog)
        {
            fprintf(fplog, "Optimized dissociation energy for %8s with %4d copies to %g\n",
                    ctest[i].c_str(), ntest[i], Edissoc[i]);
        }
    }
    free_matrix(a);
    free_matrix(a2);
}

void OptParam::InitOpt(FILE *fplog,
                       bool  bOpt[ebtsNR],
                       real  factor)
{
    for (int i = 0; (i < ebtsNR); i++)
    {
        ForceConstants_.push_back(ForceConstants(i, bOpt[i]));
        ForceConstants_[i].analyzePoldata(pd_);
        ForceConstants_[i].analyzeIdef(_mymol, pd_);
        ForceConstants_[i].makeReverseIndex();
        ForceConstants_[i].dump(fplog);
    }

    Opt2List();
    getDissociationEnergy(fplog);
    Opt2List();
    List2Opt();

    best_.resize(param_.size(), 0);
    orig_.resize(param_.size(), 0);
    lower_.resize(param_.size(), 0);
    upper_.resize(param_.size(), 0);
    psigma_.resize(param_.size(), 0);
    if (factor < 1)
    {
        factor = 1/factor;
    }
    for (size_t i = 0; (i < param_.size()); i++)
    {
        best_[i]  = orig_[i] = param_[i];
        lower_[i] = orig_[i]/factor;
        upper_[i] = orig_[i]*factor;
    }
}

void OptParam::Print(FILE *fp)
{
    fprintf(fp, "Param        Orig        Best\n");
    for (size_t k = 0; (k < param_.size()); k++)
    {
        fprintf(fp, "%-5d  %10g  %10g\n", static_cast<int>(k),
                orig_[k], best_[k]);
    }
}

static void print_stats(FILE *fp, const char *prop, gmx_stats_t lsq, gmx_bool bHeader,
                        char *xaxis, char *yaxis)
{
    real a, da, b, db, chi2, rmsd, Rfit;
    int  n;

    if (bHeader)
    {
        fprintf(fp, "Fitting data to y = ax+b, where x = %s and y = %s\n",
                xaxis, yaxis);
        fprintf(fp, "%-12s %5s %13s %13s %8s %8s\n",
                "Property", "N", "a", "b", "R", "RMSD");
        fprintf(fp, "---------------------------------------------------------------\n");
    }
    gmx_stats_get_ab(lsq, elsqWEIGHT_NONE, &a, &b, &da, &db, &chi2, &Rfit);
    gmx_stats_get_rmsd(lsq, &rmsd);
    gmx_stats_get_npoints(lsq, &n);
    fprintf(fp, "%-12s %5d %6.3f(%5.3f) %6.3f(%5.3f) %7.2f%% %8.4f\n",
            prop, n, a, da, b, db, Rfit*100, rmsd);
}

static void print_lsq_set(FILE *fp, gmx_stats_t lsq)
{
    real   x, y;

    fprintf(fp, "@type xy\n");
    while (gmx_stats_get_point(lsq, &x, &y, NULL, NULL, 0) == estatsOK)
    {
        fprintf(fp, "%10g  %10g\n", x, y);
    }
    fprintf(fp, "&\n");
}

static void xvgr_symbolize(FILE *xvgf, int nsym, const char *leg[],
                           const gmx_output_env_t *oenv)
{
    int i;

    xvgr_legend(xvgf, nsym, leg, oenv);
    for (i = 0; (i < nsym); i++)
    {
        xvgr_line_props(xvgf, i, elNone, ecBlack+i, oenv);
        fprintf(xvgf, "@ s%d symbol %d\n", i, i+1);
    }
}

double OptParam::CalcDeviation()
{
    int             j;
    int             flags;
    double          ener;
    real            t         = 0;
    rvec            mu_tot    = {0, 0, 0};
    tensor          force_vir = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    t_nrnb          my_nrnb;
    gmx_wallcycle_t wcycle;
    FILE           *dbcopy;

    if (PAR(_cr))
    {
        gmx_bcast(sizeof(_bDone), &_bDone, _cr);
        gmx_bcast(sizeof(_bFinal), &_bFinal, _cr);
    }
    if (_bDone)
    {
        return _ener[ermsTOT];
    }
    if (NULL == debug)
    {
        fprintf(debug, "Begin communicating force parameters\n");
        fflush(debug);
    }
    if (PAR(_cr))
    {
        pd_.broadcast(_cr);
    }
    if (NULL == debug)
    {
        fprintf(debug, "Done communicating force parameters\n");
        fflush(debug);
    }
    init_nrnb(&my_nrnb);

    wcycle  = wallcycle_init(stdout, 0, _cr);
    for (j = 0; (j < ermsNR); j++)
    {
        _ener[j] = 0;
    }
    flags = GMX_FORCE_NS | GMX_FORCE_LISTED | GMX_FORCE_NONBONDED | GMX_FORCE_FORCES | GMX_FORCE_ENERGY | GMX_FORCE_STATECHANGED;
    flags = ~0;
    for (auto &mymol : _mymol)
    {
        if ((mymol.eSupp == eSupportLocal) ||
            (_bFinal && (mymol.eSupp == eSupportRemote)))
        {
            /* Update topology for this molecule */
            for (const auto fc : ForceConstants_)
            {
                if (fc.nbad() > 0)
                {
                    mymol.UpdateIdef(pd_, fc.bt());
                }
            }

            /* Now compute energy */
            atoms2md(mymol.mtop_, mymol.inputrec_, 0, NULL, 0,
                     mymol.mdatoms_);

            for (j = 0; (j < mymol.molProp()->NAtom()); j++)
            {
                clear_rvec(mymol.f_[j]);
            }

            /* Now optimize the shell positions */
            dbcopy = debug;
            debug  = NULL;
            if (mymol.shellfc_)
            {
                mymol.relaxShells(_cr);
            }
            else
            {
                do_force(debug, _cr, mymol.inputrec_, 0,
                         &my_nrnb, wcycle, mymol.ltop_,
                         &(mymol.mtop_->groups),
                         mymol.box_, mymol.x_, NULL,
                         mymol.f_, force_vir, mymol.mdatoms_,
                         mymol.enerd_, NULL,
                         mymol.state_->lambda, NULL,
                         mymol.fr_,
                         NULL, mu_tot, t, NULL, NULL, FALSE,
                         flags);
            }
            debug         = dbcopy;
            mymol.Force2  = 0;
            for (j = 0; (j < mymol.molProp()->NAtom()); j++)
            {
                mymol.Force2 += iprod(mymol.f_[j], mymol.f_[j]);
            }
            mymol.Force2      /= mymol.molProp()->NAtom();
            _ener[ermsForce2] += _fc[ermsForce2]*mymol.Force2;
            mymol.Ecalc        = mymol.enerd_->term[F_EPOT];
            ener               = gmx::square(mymol.Ecalc-mymol.Emol);
            _ener[ermsEPOT]   += _fc[ermsEPOT]*ener/_nmol_support;

            if (NULL != debug)
            {
                fprintf(debug, "%s ener %g Epot %g Force2 %g\n",
                        mymol.molProp()->getMolname().c_str(), ener,
                        mymol.Ecalc, mymol.Force2);
            }
        }
    }
    /* Compute E-bounds */
    for (size_t j = 0; (j < param_.size()); j++)
    {
        if (param_[j] < lower_[j])
        {
            _ener[ermsBOUNDS] += _fc[ermsBOUNDS]*gmx::square(param_[j]-lower_[j]);
        }
        else if (param_[j] > upper_[j])
        {
            _ener[ermsBOUNDS] += _fc[ermsBOUNDS]*gmx::square(param_[j]-upper_[j]);
        }
    }

    for (j = 0; (j < ermsTOT); j++)
    {
        _ener[ermsTOT] += _ener[j];
    }

    if (debug)
    {
        fprintf(debug, "ENER:");
        for (j = 0; (j < ermsNR); j++)
        {
            fprintf(debug, "  %8.3f", _ener[j]);
        }
        fprintf(debug, "\n");
    }
    /* Global sum energies */
    if (PAR(_cr))
    {
#if GMX_DOUBLE
        gmx_sumd(ermsNR, _ener, _cr);
#else
        gmx_sumf(ermsNR, _ener, _cr);
#endif
    }
    return _ener[ermsTOT];
}

double OptParam::EnergyFunction(double v[])
{
    /* Copy parameters to topologies */
    for (size_t i = 0; (i < param_.size()); i++)
    {
        param_[i] = v[i];
    }
    List2Opt();

    return CalcDeviation();
}

static real guess_new_param(real x, real step, real x0, real x1, gmx_rng_t rng,
                            gmx_bool bRandom)
{
    real r = gmx_rng_uniform_real(rng);

    if (bRandom)
    {
        x = x0+(x1-x0)*r;
    }
    else
    {
        x = x*(1-step+2*step*r);
    }

    if (x < x0)
    {
        return x0;
    }
    else if (x > x1)
    {
        return x1;
    }
    else
    {
        return x;
    }
}

void OptParam::GuessAll(int iter, real stepsize,
                        bool bRandom, gmx_rng_t rng)
{
    double   ppp, xxx;
    gmx_bool bStart = (iter == 0);
    gmx_bool bRand  = bRandom && (iter == 0);

    for (size_t n = 0; (n < param_.size()); n++)
    {
        if (bStart)
        {
            ppp = param_[n];
            xxx = guess_new_param(ppp, stepsize, lower_[n], upper_[n], rng, bRand);
            if (bRand)
            {
                orig_[n] = xxx;
            }
            else
            {
                orig_[n] = ppp;
            }
            ppp = xxx;
        }
        else
        {
            ppp = guess_new_param(orig_[n], stepsize, lower_[n],
                                  upper_[n], rng, bRand);
        }
        param_[n] = ppp;
    }
}

void OptParam::Bayes(FILE *fplog, const char *xvgconv, const char *xvgepot,
                     int nprint,
                     double step,
                     unsigned int seed,
                     real temperature,
                     int    maxiter,
                     double *chi2,
                     const gmx_output_env_t *oenv)
{
    int       iter, j, nsum, cur = 0;
    double    ds, sorig, DE, E[2] = {0, 0}, beta;
    double   *ssum, *s2sum;
#define prev (1-cur)
    gmx_rng_t rng;
    FILE     *fpc = NULL, *fpe = NULL;

    beta = 1/(BOLTZ*temperature);
    if (NULL != xvgconv)
    {
        fpc = xvgropen(xvgconv, "Parameter convergence", "iteration", "", oenv);
    }
    if (NULL != xvgepot)
    {
        fpe = xvgropen(xvgepot, "Parameter energy", "iteration", "kT", oenv);
    }
    rng = gmx_rng_init(seed);

    E[prev] = EnergyFunction(param_.data());
    *chi2   = E[prev];
    snew(ssum, param_.size());
    snew(s2sum, param_.size());
    nsum = 0;
    for (j = iter = 0; (iter < maxiter); iter++)
    {
        if ((NULL != fpc) && ((j % nprint) == 0))
        {
            fprintf(fpc, "%5d", iter);
            for (size_t k = 0; (k < param_.size()); k++)
            {
                fprintf(fpc, "  %10g", param_[k]);
            }
            fprintf(fpc, "\n");
        }
        if ((NULL != fpe) && ((j % nprint) == 0))
        {
            fprintf(fpe, "%5d  %10g\n", iter, E[prev]);
        }
        ds         = (2*gmx_rng_uniform_real(rng)-1)*step*fabs(param_[j]);
        sorig      = param_[j];
        param_[j] += ds;
        E[cur]     = EnergyFunction(param_.data());
        DE         = E[cur]-E[prev];
        if (NULL != debug)
        {
            fprintf(debug, "DE = %g ds = %g\n", DE, ds);
        }
        if ((DE < 0) || (exp(-beta*DE) > gmx_rng_uniform_real(rng)))
        {
            if (NULL != debug)
            {
                fprintf(debug, "Changing parameter %3d from %.3f to %.3f. DE = %.3f 'kT'\n",
                        j, sorig, param_[j], beta*DE);
            }
            *chi2 = E[cur];
            cur   = prev;
        }
        else
        {
            param_[j] = sorig;
        }
        if (iter >= maxiter/2)
        {
            for (size_t k = 0; (k < param_.size()); k++)
            {
                ssum[k]  += param_[k];
                s2sum[k] += gmx::square(param_[k]);
            }
            nsum++;
        }
        j = (j+1) % param_.size();
    }
    gmx_rng_destroy(rng);
    if (NULL != fpc)
    {
        xvgrclose(fpc);
    }
    if (NULL != fpe)
    {
        xvgrclose(fpe);
    }
    if (nsum > 0)
    {
        for (size_t k = 0; (k < param_.size()); k++)
        {
            ssum[k]  /= nsum;
            s2sum[k] /= nsum;
        }
    }
    if (NULL != fplog)
    {
        fprintf(fplog, "Average and standard deviation of parameters\n");
        for (size_t k = 0; (k < param_.size()); k++)
        {
            psigma_[k] = sqrt(s2sum[k]-gmx::square(ssum[k]));
            fprintf(fplog, "%5d  %10g  %10g\n",
                    static_cast<int>(k), ssum[k], psigma_[k]);
            param_[k] = ssum[k];
        }
    }
    sfree(ssum);
    sfree(s2sum);
}

void OptParam::Optimize(FILE *fp, FILE *fplog,
                        int maxiter,
                        int nrun, real stepsize, int seed,
                        bool bRandom, const gmx_output_env_t *oenv,
                        int nprint,
                        const char *xvgconv, const char *xvgepot,
                        real temperature)
{
    double    chi2, chi2_min;
    int       n;
    gmx_bool  bMinimum = FALSE;
    gmx_rng_t rng;

    if (MASTER(_cr))
    {
        rng = gmx_rng_init(seed);

        chi2 = chi2_min = GMX_REAL_MAX;
        for (n = 0; (n < nrun); n++)
        {
            if ((NULL != fp) && (0 == n))
            {
                fprintf(fp, "\nStarting run %d out of %d\n", n+1, nrun);
            }

            GuessAll(n, stepsize, bRandom, rng);

            Bayes(fplog, xvgconv, xvgepot, nprint,
                  stepsize, seed, temperature, maxiter, &chi2, oenv);

            if (chi2 < chi2_min)
            {
                bMinimum = TRUE;
                /* Print convergence if needed */
                for (size_t k = 0; (k < param_.size()); k++)
                {
                    best_[k] = param_[k];
                }
                chi2_min   = chi2;
            }

            if (NULL != fp)
            {
                fprintf(fp, "%5d  %8.3f  %8.3f  %8.3f\n", n, chi2, _ener[ermsTOT], _ener[ermsBOUNDS]);
            }
            if (NULL != fplog)
            {
                fprintf(fplog, "%5d  %8.3f  %8.3f  %8.3f\n", n, chi2, _ener[ermsTOT], _ener[ermsBOUNDS]);
                fflush(fplog);
            }
        }

        if (bMinimum)
        {
            param_ = best_;

            double emin = EnergyFunction(best_.data());
            if (fplog)
            {
                fprintf(fplog, "\nMinimum chi^2 value during optimization: %.3f.\n",
                        chi2_min);
                fprintf(fplog, "\nMinimum RMSD value during optimization: %.3f (kJ/mol).\n",
                        emin);
                //print_opt(fplog,opt);
            }
        }
        CalcDeviation();
        _bDone = TRUE;
        gmx_rng_destroy(rng);
    }
    else
    {
        /* Slave calculators */
        do
        {
            CalcDeviation();
        }
        while (!_bDone);
    }
    CalcDeviation();
}

static void print_moldip_mols(FILE *fp, std::vector<alexandria::MyMol> mol,
                              gmx_bool bForce, gmx_bool bMtop)
{
    int j, k;

    for (std::vector<alexandria::MyMol>::iterator mi = mol.begin(); (mi < mol.end()); mi++)
    {
        fprintf(fp, "%-30s  %d\n", mi->molProp()->getMolname().c_str(), mi->molProp()->NAtom());
        for (j = 0; (j < mi->molProp()->NAtom()); j++)
        {
            fprintf(fp, "  %-5s  %-5s  q = %10g", *(mi->topology_->atoms.atomname[j]),
                    *(mi->topology_->atoms.atomtype[j]), mi->topology_->atoms.atom[j].q);
            if (bForce)
            {
                fprintf(fp, "  %8.3f  %8.3f  %8.3f",
                        mi->f_[j][XX],
                        mi->f_[j][YY],
                        mi->f_[j][ZZ]);
            }
            fprintf(fp, "\n");
        }
        if (bForce)
        {
            for (k = 0; (k < F_NRE); k++)
            {
                if ((mi->enerd_->term[k] != 0) ||
                    (mi->mtop_->moltype[0].ilist[k].nr > 0))
                {
                    fprintf(fp, "%s %d %g\n", interaction_function[k].name,
                            mi->mtop_->moltype[0].ilist[k].nr,
                            mi->enerd_->term[k]);
                }
            }
        }
        if (bMtop)
        {
            pr_mtop(fp, 0, mi->molProp()->getMolname().c_str(), mi->mtop_, TRUE);
        }
    }
}

void OptParam::PrintSpecs(FILE *fp, char *title,
                          const char *xvg, const gmx_output_env_t *oenv,
                          bool bCheckOutliers)
{
    FILE       *xfp;
    int         i;
    double      msd;
    gmx_stats_t gst;

    gst = gmx_stats_init();
    if (NULL != xvg)
    {
        xfp = xvgropen(xvg, "Entalpy of Formation", "Experiment (kJ/mol)", "Calculated (kJ/mol)",
                       oenv);
    }
    fprintf(fp, "%s\n", title);
    fprintf(fp, "Nr.   %-30s %10s %10s %10s %10s %10s\n",
            "Molecule", "DHf@298K", "Emol@0K", "Calc-Exp", "rms F", "Outlier?");
    msd = 0;
    i   = 0;
    for (std::vector<alexandria::MyMol>::iterator mi = _mymol.begin();
         (mi < _mymol.end()); mi++, i++)
    {
        real DeltaE = mi->Ecalc - mi->Emol;
        fprintf(fp, "%-5d %-30s %10g %10g %10g %10g %-10s\n",
                i,
                mi->molProp()->getMolname().c_str(),
                mi->Hform, mi->Emol, DeltaE,
                sqrt(mi->Force2),
                (bCheckOutliers && (fabs(DeltaE) > 1000)) ? "XXX" : "");
        msd += gmx::square(mi->Emol-mi->Ecalc);
        gmx_stats_add_point(gst, mi->Hform, mi->Hform + DeltaE, 0, 0);
        if (NULL != xvg)
        {
            fprintf(xfp, "%10g  %10g\n", mi->Hform, mi->Hform + DeltaE);
        }
    }
    fprintf(fp, "\n");
    fprintf(fp, "RMSD is %g kJ/mol for %d molecules.\n\n",
            sqrt(msd/_mymol.size()), static_cast<int>(_mymol.size()));
    fflush(fp);
    if (NULL != xvg)
    {
        xvgrclose(xfp);
        do_view(oenv, xvg, NULL);
    }
    //! Do statistics
    real a, b, da, db, chi2, Rfit;
    int  N;
    gmx_stats_get_ab(gst, 1, &a, &b, &da, &db, &chi2, &Rfit);
    gmx_stats_get_npoints(gst, &N);
    fprintf(fp, "Regression analysis fit to y = ax + b:\n");
    fprintf(fp, "a = %.3f  b = %3f  R2 = %.1f%%  chi2 = %.1f N = %d\n",
            a, b, Rfit*100, chi2, N);
    gmx_stats_free(gst);
    fflush(fp);
}
}

int alex_tune_fc(int argc, char *argv[])
{
    static const char    *desc[] = {
        "tune_fc read a series of molecules and corresponding experimental",
        "heats of formation from a file, and tunes parameters in an algorithm",
        "until the experimental energies are reproduced by the force field.[PAR]",
        "Minima and maxima for the parameters can be set, these are however",
        "not strictly enforced, but rather they are penalized with a harmonic",
        "function, for which the force constant can be set explicitly.[PAR]",
        "At every reinit step parameters are changed by a random amount within",
        "the fraction set by step size, and within the boundaries given",
        "by the minima and maxima. If the [TT]-random[tt] flag is",
        "given a completely random set of parameters is generated at the start",
        "of each run. At reinit steps however, the parameters are only changed",
        "slightly, in order to speed-up local search but not global search."
        "In other words, complete random starts are done only at the beginning of each",
        "run, and only when explicitly requested.[PAR]",
        "The absolut dipole moment of a molecule remains unchanged if all the",
        "atoms swap the sign of the charge. To prevent this kind of mirror",
        "effects a penalty is added to the square deviation ",
        "if hydrogen atoms have a negative charge. Similarly a penalty is",
        "added if atoms from row VI or VII in the periodic table have a positive",
        "charge. The penalty is equal to the force constant given on the command line",
        "time the square of the charge.[PAR]",
        "One of the electronegativities (chi) is redundant in the optimization,",
        "only the relative values are meaningful.",
        "Therefore by default we fix the value for hydrogen to what is written",
        "in the eemprops.dat file (or whatever is given with the [tt]-d[TT] flag).",
        "A suitable value would be 2.3, the original, value due to Pauling,",
        "this can by overridden by setting the [tt]-fixchi[TT] flag to something else (e.g. a non-existing atom).[PAR]",
        "A selection of molecules into a training set and a test set (or ignore set)",
        "can be made using option [TT]-sel[tt]. The format of this file is:[BR]",
        "iupac|Train[BR]",
        "iupac|Test[BR]",
        "iupac|Ignore[BR]",
        "and you should ideally have a line for each molecule in the molecule database",
        "([TT]-f[tt] option). Missing molecules will be ignored."
    };

    t_filenm              fnm[] = {
        { efDAT, "-f", "allmols",    ffREAD  },
        { efDAT, "-d", "gentop",     ffOPTRD },
        { efDAT, "-o", "tune_fc",    ffWRITE },
        { efDAT, "-sel", "molselect", ffREAD },
        { efXVG, "-tab", "table",    ffOPTRD },
        { efLOG, "-g", "tune_fc",    ffWRITE },
        { efXVG, "-x", "hform-corr", ffWRITE },
        { efXVG, "-conv", "param-conv", ffWRITE },
        { efXVG, "-epot", "param-epot", ffWRITE }
    };
#define NFILE sizeof(fnm)/sizeof(fnm[0])
    static int            nrun          = 1, maxiter = 100, reinit = 0, seed = 0;
    static int            minimum_data  = 3, compress = 0;
    static real           tol           = 1e-3, stol = 1e-6, watoms = 0;
    static gmx_bool       bRandom       = FALSE, bZero = TRUE, bWeighted = TRUE, bOptHfac = FALSE, bQM = FALSE, bGaussianBug = TRUE, bPol = FALSE, bFitZeta = TRUE;
    static real           J0_0          = 5, Chi0_0 = 1, w_0 = 5, step = 0.01, hfac = 0, rDecrZeta = -1;
    static real           J0_1          = 30, Chi0_1 = 30, w_1 = 50;
    static real           fc_mu         = 1, fc_bound = 1, fc_quad = 1, fc_charge = 0, fc_esp = 0, fc_epot = 1, fc_force = 0.001;
    static real           factor        = 0.8;
    static char          *opt_elem      = NULL, *const_elem = NULL, *fixchi = (char *)"H";
    static char          *lot           = (char *)"B3LYP/aug-cc-pVTZ";
    static const char    *cqdist[]      = {
        NULL, "AXp", "AXg", "AXs",
        "Yang", "Bultinck", "Rappe", NULL
    };
    static const char    *cqgen[]      = {
        NULL, "None", "EEM", "ESP", "RESP", NULL
    };
    static bool           bOpt[ebtsNR] = { true, false, false, false, false, false };
    static real           beta0        = 0, D0 = 0, beta_min = 10, D0_min = 50, temperature;
    static int            nprint       = 10;
    t_pargs               pa[]         = {
        { "-tol",   FALSE, etREAL, {&tol},
          "Tolerance for convergence in optimization" },
        { "-maxiter", FALSE, etINT, {&maxiter},
          "Max number of iterations for optimization" },
        { "-nprint", FALSE, etINT, {&nprint},
          "How often to print the parameters during the simulation" },
        { "-temp",  FALSE, etREAL, {&temperature},
          "'Temperature' for the Monte Carlo simulation" },
        { "-reinit", FALSE, etINT, {&reinit},
          "After this many iterations the search vectors are randomized again. A vlue of 0 means this is never done at all." },
        { "-stol",   FALSE, etREAL, {&stol},
          "If reinit is -1 then a reinit will be done as soon as the simplex size is below this treshold." },
        { "-nrun",   FALSE, etINT,  {&nrun},
          "This many runs will be done, before each run a complete randomization will be done" },
        { "-qdist",   FALSE, etENUM, {cqdist},
          "Model used for charge distribution" },
        { "-qgen",   FALSE, etENUM, {cqgen},
          "Algorithm used for charge generation" },
        { "-bonds",  FALSE, etBOOL, {&bOpt[ebtsBONDS]},
          "Optimize bond parameters" },
        { "-angles",  FALSE, etBOOL, {&bOpt[ebtsANGLES]},
          "Optimize angle parameters" },
        { "-dihedrals",  FALSE, etBOOL, {&bOpt[ebtsPDIHS]},
          "Optimize proper dihedral parameters" },
        { "-impropers",  FALSE, etBOOL, {&bOpt[ebtsIDIHS]},
          "Optimize improper dihedral parameters" },
        { "-beta0", FALSE, etREAL, {&beta0},
          "Reset the initial beta for Morse potentials to this value, independent of gentop.dat. If value is <= 0 gentop.dat value is used." },
        { "-D0", FALSE, etREAL, {&D0},
          "Reset the initial D for Morse potentials to this value, independent of gentop.dat. If value is <= 0 gentop.dat value is used." },
        { "-beta_min", FALSE, etREAL, {&beta_min},
          "Minimum value for beta in Morse potential" },
        { "-DO_min", FALSE, etREAL, {&D0_min},
          "Minimum value for D0 in Morse potential" },
        { "-qm",     FALSE, etBOOL, {&bQM},
          "Use only quantum chemistry results (from the levels of theory below) in order to fit the parameters. If not set, experimental values will be used as reference with optional quantum chemistry results, in case no experimental results are available" },
        { "-lot",    FALSE, etSTR,  {&lot},
          "Use this method and level of theory when selecting coordinates and charges. Multiple levels can be specified which will be used in the order given, e.g.  B3LYP/aug-cc-pVTZ:HF/6-311G**" },
        { "-fc_bound",    FALSE, etREAL, {&fc_bound},
          "Force constant in the penalty function for going outside the borders given with the above six options." },
        { "-fc_epot",    FALSE, etREAL, {&fc_epot},
          "Force constant in the penalty function for the energy term" },
        { "-fc_force",    FALSE, etREAL, {&fc_force},
          "Force constant in the penalty function for the force term" },
        { "-step",  FALSE, etREAL, {&step},
          "Step size in parameter optimization. Is used as a fraction of the starting value, should be less than 10%. At each reinit step the step size is updated." },
        { "-min_data",  FALSE, etINT, {&minimum_data},
          "Minimum number of data points in order to be able to optimize the parameters for a given atomtype" },
        { "-opt_elem",  FALSE, etSTR, {&opt_elem},
          "Space-separated list of elements to optimize, e.g. \"H C Br\". The other available elements in gentop.dat are left unmodified. If this variable is not set, all elements will be optimized." },
        { "-const_elem",  FALSE, etSTR, {&const_elem},
          "Space-separated list of elements to include but keep constant, e.g. \"O N\". These elements from gentop.dat are left unmodified" },
        { "-seed", FALSE, etINT, {&seed},
          "Random number seed for reinit" },
        { "-factor", FALSE, etREAL, {&factor},
          "Factor for generating random parameters. Parameters will be taken within the limit factor*x - x/factor" },
        { "-random", FALSE, etBOOL, {&bRandom},
          "Generate completely random starting parameters within the limits set by the options. This will be done at the very first step and before each subsequent run." },
        { "-weight", FALSE, etBOOL, {&bWeighted},
          "Perform a weighted fit, by using the errors in the dipoles presented in the input file. This may or may not improve convergence." },
        { "-compress", FALSE, etBOOL, {&compress},
          "Compress output XML file" }
    };
    FILE                 *fp;
    t_commrec            *cr;
    gmx_output_env_t     *oenv;
    time_t                my_t;

    cr = init_commrec();
    if (MASTER(cr))
    {
        printf("There are %d threads/processes.\n", cr->nnodes);
    }
    if (!parse_common_args(&argc, argv, PCA_CAN_VIEW,
                           NFILE, fnm,
                           sizeof(pa)/sizeof(pa[0]), pa,
                           sizeof(desc)/sizeof(desc[0]), desc,
                           0, NULL, &oenv))
    {
        return 0;
    }

    if (MASTER(cr))
    {
        fp = gmx_ffopen(opt2fn("-g", NFILE, fnm), "w");

        time(&my_t);
        fprintf(fp, "# This file was created %s", ctime(&my_t));
        fprintf(fp, "# alexandria is part of G R O M A C S:\n#\n");
        fprintf(fp, "# %s\n#\n", gmx::bromacs().c_str());
    }
    else
    {
        fp = NULL;
    }

    MolSelect gms;
    if (MASTER(cr))
    {
        gms.read(opt2fn_null("-sel", NFILE, fnm));
    }

    alexandria::OptParam      opt;
    ChargeDistributionModel   iDistributionModel         = name2eemtype(cqdist[0]);
    ChargeGenerationAlgorithm iChargeGenerationAlgorithm = (ChargeGenerationAlgorithm) get_option(cqgen);

    opt.Init(cr, bQM, bGaussianBug, iDistributionModel,
             iChargeGenerationAlgorithm,
             rDecrZeta,
             J0_0, Chi0_0, w_0, J0_1, Chi0_1, w_1,
             fc_bound, fc_mu, fc_quad, fc_charge,
             fc_esp, fc_epot, fc_force, fixchi, bOptHfac, hfac, bPol, bFitZeta);
    if (0 == seed)
    {
        seed = gmx_rng_make_seed();
    }
    opt.Read(fp ? fp : (debug ? debug : NULL),
             opt2fn("-f", NFILE, fnm),
             opt2fn_null("-d", NFILE, fnm),
             minimum_data, bZero,
             opt_elem, const_elem,
             lot, gms, watoms, FALSE,
             false, bPol,
             opt2fn_null("-tab", NFILE, fnm));

    opt.checkSupport(fp, bOpt);

    fprintf(fp, "In the total data set of %d molecules we have:\n",
            static_cast<int>(opt._mymol.size()));


    opt.InitOpt(fp, bOpt, factor);

    print_moldip_mols(fp, opt._mymol, FALSE, FALSE);
    if (MASTER(cr))
    {
        opt.PrintSpecs(fp, (char *)"Before optimization", NULL, oenv, false);
    }
    opt.Optimize(MASTER(cr) ? stderr : NULL, fp,
                 maxiter, nrun, step, seed,
                 bRandom, oenv, nprint,
                 opt2fn("-conv", NFILE, fnm),
                 opt2fn("-epot", NFILE, fnm),
                 temperature);
    if (MASTER(cr))
    {
        print_moldip_mols(fp, opt._mymol, TRUE, FALSE);
        opt.PrintSpecs(fp, (char *)"After optimization",
                       opt2fn("-x", NFILE, fnm), oenv, true);

        writePoldata(opt2fn("-o", NFILE, fnm), opt.pd_, compress);

        done_filenms(NFILE, fnm);

        gmx_ffclose(fp);
    }

    return 0;
}
