#include <cmath>

#include "../gmx_blas.h"
#include "../gmx_lapack.h"

#include "gromacs/utility/real.h"

#include "lapack_limits.h"

void F77_FUNC(dstein, DSTEIN)(int*    n,
                              double* d__,
                              double* e,
                              int*    m,
                              double* w,
                              int*    iblock,
                              int*    isplit,
                              double* z__,
                              int*    ldz,
                              double* work,
                              int*    iwork,
                              int*    ifail,
                              int*    info)
{
    int    z_dim1, z_offset, i__1, i__2, i__3;
    double d__2, d__3, d__4, d__5;

    int    i__, j, b1, j1, bn;
    double xj, scl, eps, sep, nrm, tol;
    int    its;
    double xjm, ztr, eps1;
    int    jblk, nblk;
    int    jmax;

    int    iseed[4], gpind, iinfo;
    double ortol;
    int    indrv1, indrv2, indrv3, indrv4, indrv5;
    int    nrmchk;
    int    blksiz;
    double onenrm, dtpcrt, pertol;
    int    c__2 = 2;
    int    c__1 = 1;
    int    c_n1 = -1;

    --d__;
    --e;
    --w;
    --iblock;
    --isplit;
    z_dim1   = *ldz;
    z_offset = 1 + z_dim1;
    z__ -= z_offset;
    --work;
    --iwork;
    --ifail;

    *info = 0;

    xjm  = 0.0;
    i__1 = *m;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        ifail[i__] = 0;
    }

    if (*n < 0)
    {
        *info = -1;
    }
    else if (*m < 0 || *m > *n)
    {
        *info = -4;
    }
    else if (*ldz < (*n))
    {
        *info = -9;
    }
    else
    {
        i__1 = *m;
        for (j = 2; j <= i__1; ++j)
        {
            if (iblock[j] < iblock[j - 1])
            {
                *info = -6;
                break;
            }
            if (iblock[j] == iblock[j - 1] && w[j] < w[j - 1])
            {
                *info = -5;
                break;
            }
        }
    }

    if (*info != 0)
    {
        return;
    }

    if (*n == 0 || *m == 0)
    {
        return;
    }
    else if (*n == 1)
    {
        z__[z_dim1 + 1] = 1.;
        return;
    }

    eps = GMX_DOUBLE_EPS;

    for (i__ = 1; i__ <= 4; ++i__)
    {
        iseed[i__ - 1] = 1;
    }

    indrv1 = 0;
    indrv2 = indrv1 + *n;
    indrv3 = indrv2 + *n;
    indrv4 = indrv3 + *n;
    indrv5 = indrv4 + *n;

    j1   = 1;
    i__1 = iblock[*m];
    for (nblk = 1; nblk <= i__1; ++nblk)
    {

        if (nblk == 1)
        {
            b1 = 1;
        }
        else
        {
            b1 = isplit[nblk - 1] + 1;
        }
        bn     = isplit[nblk];
        blksiz = bn - b1 + 1;
        if (blksiz == 1)
        {
            continue;
        }
        gpind = b1;

        onenrm = std::abs(d__[b1]) + std::abs(e[b1]);
        d__3   = onenrm;
        d__4   = std::abs(d__[bn]) + std::abs(e[bn - 1]);
        onenrm = (d__3 > d__4) ? d__3 : d__4;
        i__2   = bn - 1;
        for (i__ = b1 + 1; i__ <= i__2; ++i__)
        {
            d__4   = onenrm;
            d__5   = std::abs(d__[i__]) + std::abs(e[i__ - 1]) + std::abs(e[i__]);
            onenrm = (d__4 > d__5) ? d__4 : d__5;
        }
        ortol = onenrm * .001;

        dtpcrt = std::sqrt(.1 / blksiz);

        jblk = 0;
        i__2 = *m;
        for (j = j1; j <= i__2; ++j)
        {
            if (iblock[j] != nblk)
            {
                j1 = j;
                break;
            }
            ++jblk;
            xj = w[j];

            if (blksiz == 1)
            {
                work[indrv1 + 1] = 1.;
                goto L120;
            }

            if (jblk > 1)
            {
                eps1   = std::abs(eps * xj);
                pertol = eps1 * 10.;
                sep    = xj - xjm;
                if (sep < pertol)
                {
                    xj = xjm + pertol;
                }
            }

            its    = 0;
            nrmchk = 0;

            F77_FUNC(dlarnv, DLARNV)(&c__2, iseed, &blksiz, &work[indrv1 + 1]);

            F77_FUNC(dcopy, DCOPY)(&blksiz, &d__[b1], &c__1, &work[indrv4 + 1], &c__1);
            i__3 = blksiz - 1;
            F77_FUNC(dcopy, DCOPY)(&i__3, &e[b1], &c__1, &work[indrv2 + 2], &c__1);
            i__3 = blksiz - 1;
            F77_FUNC(dcopy, DCOPY)(&i__3, &e[b1], &c__1, &work[indrv3 + 1], &c__1);

            tol = 0.;
            F77_FUNC(dlagtf, DLAGTF)
            (&blksiz,
             &work[indrv4 + 1],
             &xj,
             &work[indrv2 + 2],
             &work[indrv3 + 1],
             &tol,
             &work[indrv5 + 1],
             &iwork[1],
             &iinfo);

        L70:
            ++its;
            if (its > 5)
            {
                goto L100;
            }

            d__2 = eps;
            d__3 = std::abs(work[indrv4 + blksiz]);
            scl  = blksiz * onenrm * ((d__2 > d__3) ? d__2 : d__3)
                  / F77_FUNC(dasum, DASUM)(&blksiz, &work[indrv1 + 1], &c__1);
            F77_FUNC(dscal, DSCAL)(&blksiz, &scl, &work[indrv1 + 1], &c__1);

            F77_FUNC(dlagts, DLAGTS)
            (&c_n1,
             &blksiz,
             &work[indrv4 + 1],
             &work[indrv2 + 2],
             &work[indrv3 + 1],
             &work[indrv5 + 1],
             &iwork[1],
             &work[indrv1 + 1],
             &tol,
             &iinfo);

            if (jblk == 1)
            {
                goto L90;
            }
            if (std::abs(xj - xjm) > ortol)
            {
                gpind = j;
            }
            if (gpind != j)
            {
                i__3 = j - 1;
                for (i__ = gpind; i__ <= i__3; ++i__)
                {
                    ztr = -F77_FUNC(ddot, DDOT)(
                            &blksiz, &work[indrv1 + 1], &c__1, &z__[b1 + i__ * z_dim1], &c__1);
                    F77_FUNC(daxpy, DAXPY)
                    (&blksiz, &ztr, &z__[b1 + i__ * z_dim1], &c__1, &work[indrv1 + 1], &c__1);
                }
            }

        L90:
            jmax = F77_FUNC(idamax, IDAMAX)(&blksiz, &work[indrv1 + 1], &c__1);
            nrm  = std::abs(work[indrv1 + jmax]);

            if (nrm < dtpcrt)
            {
                goto L70;
            }
            ++nrmchk;
            if (nrmchk < 3)
            {
                goto L70;
            }

            goto L110;

        L100:
            ++(*info);
            ifail[*info] = j;

        L110:
            scl  = 1. / F77_FUNC(dnrm2, DNRM2)(&blksiz, &work[indrv1 + 1], &c__1);
            jmax = F77_FUNC(idamax, IDAMAX)(&blksiz, &work[indrv1 + 1], &c__1);
            if (work[indrv1 + jmax] < 0.)
            {
                scl = -scl;
            }
            F77_FUNC(dscal, DSCAL)(&blksiz, &scl, &work[indrv1 + 1], &c__1);
        L120:
            i__3 = *n;
            for (i__ = 1; i__ <= i__3; ++i__)
            {
                z__[i__ + j * z_dim1] = 0.;
            }
            i__3 = blksiz;
            for (i__ = 1; i__ <= i__3; ++i__)
            {
                z__[b1 + i__ - 1 + j * z_dim1] = work[indrv1 + i__];
            }

            xjm = xj;
        }
    }

    return;
}
