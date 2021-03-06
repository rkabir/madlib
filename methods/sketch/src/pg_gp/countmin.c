/*!
 * \file countmin.c
 *
 * \brief CountMin sketch implementation
 *
 * \implementation
 * The basic CountMin sketch is a set of DEPTH arrays, each with NUMCOUNTERS counters.
 * The idea is that each of those arrays is used as an independent random trial of the
 * same process: for all the values x in a set, each holds counts of h_i(x) mod NUMCOUNTERS for a different random hash function h_i.
 * Estimates of the count of some value x are based on the <i>minimum</i> counter h_i(x) across
 * the DEPTH arrays (hence the name CountMin.)
 *
 * Let's call the process described above "sketching" the x's.  To support range
 * lookups, we repeat the basic CountMin sketching process INT64BITS times as follows.
 * (This is the "dyadic range" trick mentioned in Cormode/Muthu.)
 *
 * Every value x/(2^i) is sketched
 * at a different power-of-2 (dyadic) "range" i.  So we sketch x in range 0, then sketch x/2
 * in range 1, then sketch x/4 in range 2, etc.
 * This allows us to count up ranges (like 14-48) by doing CountMin equality lookups on constituent
 * dyadic ranges ({[14-15] as 7 in range 2, [16-31] as 1 in range 16, [32-47] as 2 in range 16, [48-48] as 48 in range 1}).
 * Dyadic ranges are similarly useful for histogramming, order stats, etc.
 *
 * The results of the estimators below generally have guarantees of the form
 * "the answer is within \epsilon of the true answer with probability 1-\delta."
 */

#include "postgres.h"
#include "utils/array.h"
#include "utils/elog.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "nodes/execnodes.h"
#include "fmgr.h"
#include "sketch_support.h"
#include "catalog/pg_type.h"
#include "countmin.h"

#include <ctype.h>

PG_FUNCTION_INFO_V1(__cmsketch_int8_trans);

/*
 * This is the UDF interface.  It does sanity checks and preps values
 * for the interesting logic in countmin_dyadic_trans_c
 */
Datum __cmsketch_int8_trans(PG_FUNCTION_ARGS)
{
    bytea *     transblob = NULL;
    cmtransval *transval;

    /*
     * This function makes destructive updates to its arguments.
     * Make sure it's being called in an agg context.
     */

    if (!(fcinfo->context &&
          (IsA(fcinfo->context, AggState)
    #ifdef NOTGP
           || IsA(fcinfo->context, WindowAggState)
    #endif
          )))
        elog(ERROR,
             "destructive pass by reference outside agg");

    /* get the provided element, being careful in case it's NULL */
    if (!PG_ARGISNULL(1)) {
        transblob = cmsketch_check_transval(fcinfo, true);
        transval = (cmtransval *)(VARDATA(transblob));

        /* the following line modifies the contents of transval, and hence transblob */
        countmin_dyadic_trans_c(transval, PG_GETARG_DATUM(1));
        PG_RETURN_DATUM(PointerGetDatum(transblob));
    }
    else PG_RETURN_DATUM(PointerGetDatum(PG_GETARG_BYTEA_P(0)));
}

/*!
 * check if the transblob is not initialized, and do so if not
 * \param transblob a cmsketch transval packed in a bytea
 */
bytea *cmsketch_check_transval(PG_FUNCTION_ARGS, bool initargs)
{
    bytea *     transblob = PG_GETARG_BYTEA_P(0);
    cmtransval *transval;
    Oid         element_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

    /*
     * an uninitialized transval should be a datum smaller than sizeof(cmtransval).
     * if this one is small, initialize it now, else return it.
     */
    if (!CM_TRANSVAL_INITIALIZED(transblob)) {
        /* XXX would be nice to pfree the existing transblob, but pfree complains. */
        transblob = cmsketch_init_transval(element_type);
        transval = (cmtransval *)VARDATA(transblob);

        if (initargs) {
            int nargs = PG_NARGS();
            int i;
            /* carry along any additional args */
            if (nargs - 2 > MAXARGS)
                elog(
                    ERROR,
                    "no more than %d additional arguments should be passed to __cmsketch_int8_trans",
                    MAXARGS);
            transval->nargs = nargs - 2;
            for (i = 2; i < nargs; i++) {
                if (PG_ARGISNULL(i))
                    elog(ERROR,
                         "NULL parameter %d passed to __cmsketch_int8_trans",
                         i);
                transval->args[i-2] = PG_GETARG_DATUM(i);
            }
        }
        else transval->nargs = -1;
    }
    return(transblob);
}

bytea *cmsketch_init_transval(Oid typOid)
{
    bool        typIsVarlena;
    cmtransval *transval;

    /* allocate and zero out a transval via palloc0 */
    bytea *     transblob = (bytea *)palloc0(CM_TRANSVAL_SZ);
    SET_VARSIZE(transblob, CM_TRANSVAL_SZ);

    transval = (cmtransval *)VARDATA(transblob);
    transval->typOid = typOid;
    getTypeOutputInfo(transval->typOid,
                      &(transval->outFuncOid),
                      &typIsVarlena);
    return(transblob);
}

/*!
 * perform multiple sketch insertions, one for each dyadic range (from 0 up to RANGES-1).
 * * \param transval the cmsketch transval
 * * \param inputi the value to be inserted
 */
void countmin_dyadic_trans_c(cmtransval *transval, Datum input)
{
    uint32 j;
    
    if (transval->typOid != INT8OID)
        elog(ERROR, "cmsketch can only compute ranges for int64");

    for (j = 0; j < RANGES; j++) {
        countmin_trans_c(transval->sketches[j], input, 
                         transval->outFuncOid, transval->typOid);
        /* now divide by 2 for the next dyadic range */
        input = Int64GetDatum(DatumGetInt64(input) >> 1);
    }
}

/*!
 * Main loop of Cormode and Muthukrishnan's sketching algorithm, for setting counters in
 * sketches at a single "dyadic range". For each call, we want to use DEPTH independent
 * hash functions.  We do this by using a single md5 hash function, and taking
 * successive 16-bit runs of the result as independent hash outputs.
 * \param sketch the current countmin sketch
 * \param dat the datum to be inserted
 * \param outFuncOid Oid of the PostgreSQL function to convert dat to a string
 * \param typOid Oid of the Postgres type for dat
 */
Datum countmin_trans_c(countmin sketch, Datum dat, Oid outFuncOid, Oid typOid)
{
    bytea *nhash;
    // char *input;

    nhash = sketch_md5_bytea(dat, typOid);

    /*
     * iterate through all sketches, incrementing the counters indicated by the hash
     * we don't care about return value here, so 3rd (initialization) argument is arbitrary.
     */
    (void)hash_counters_iterate(nhash, sketch, 0, &increment_counter);
    return(PointerGetDatum(nhash));
}

/*
 * FINAL functions for various UDAs built on countmin sketches
 */

/*!
 * simply returns its input
 */
PG_FUNCTION_INFO_V1(__cmsketch_final);
Datum __cmsketch_final(PG_FUNCTION_ARGS)
{
    PG_RETURN_BYTEA_P(PG_GETARG_BYTEA_P(0));
}

/*!
 *  use sketch to get the count of the 1st arg
 */
PG_FUNCTION_INFO_V1(__cmsketch_count_final);
Datum __cmsketch_count_final(PG_FUNCTION_ARGS)
{
    bytea *     blob = PG_GETARG_BYTEA_P(0);
    cmtransval *sketch = (cmtransval *)VARDATA(blob);
    if (!CM_TRANSVAL_INITIALIZED(blob))
        PG_RETURN_NULL();
    PG_RETURN_DATUM(cmsketch_count_c(sketch->sketches[0],
                                     sketch->args[0], sketch->outFuncOid,
                                     sketch->typOid));
}

/*!
 *  use sketch to get the count in range of the 1st 2 args
 */
PG_FUNCTION_INFO_V1(__cmsketch_rangecount_final);
Datum __cmsketch_rangecount_final(PG_FUNCTION_ARGS)
{
    bytea *     blob = PG_GETARG_BYTEA_P(0);
    cmtransval *sketch = (cmtransval *)VARDATA(blob);
    if (!CM_TRANSVAL_INITIALIZED(blob))
        PG_RETURN_NULL();
    PG_RETURN_INT64(cmsketch_rangecount_c(sketch, DatumGetInt64(sketch->args[0]),
                                          DatumGetInt64(sketch->args[1])));
}

/*!
 *  use sketch to get the centile stored in the 1st arg
 */
PG_FUNCTION_INFO_V1(__cmsketch_centile_final);
Datum __cmsketch_centile_final(PG_FUNCTION_ARGS)
{
    bytea *     blob = PG_GETARG_BYTEA_P(0);
    int64       total;

    cmtransval *sketch = (cmtransval *)VARDATA(blob);
    if (!CM_TRANSVAL_INITIALIZED(blob))
        PG_RETURN_NULL();
    total = cmsketch_rangecount_c(sketch, MIN_INT64, MAX_INT64);     /* count(*) */
    if (total == 0) {
        PG_RETURN_NULL();
    }
    PG_RETURN_INT64(cmsketch_centile_c(sketch, DatumGetInt64(sketch->args[0]),
                                       total));
}

/*!
 *  use sketch to get the median
 */
PG_FUNCTION_INFO_V1(__cmsketch_median_final);
Datum __cmsketch_median_final(PG_FUNCTION_ARGS)
{
    bytea *     blob = PG_GETARG_BYTEA_P(0);
    int64       total;

    cmtransval *sketch = (cmtransval *)VARDATA(blob);
    if (!CM_TRANSVAL_INITIALIZED(blob))
        PG_RETURN_NULL();
    total = cmsketch_rangecount_c(sketch, MIN_INT64, MAX_INT64);     /* count(*) */
    if (total == 0) {
        PG_RETURN_NULL();
    }
    PG_RETURN_INT64(cmsketch_centile_c(sketch, 50, total));
}

/*!
 *  use sketch get the equi-depth histogram
 */
PG_FUNCTION_INFO_V1(__cmsketch_dhist_final);
Datum __cmsketch_dhist_final(PG_FUNCTION_ARGS)
{
    bytea *     blob = PG_GETARG_BYTEA_P(0);
    cmtransval *sketch = (cmtransval *)VARDATA(blob);
    if (!CM_TRANSVAL_INITIALIZED(blob))
        PG_RETURN_NULL();
    PG_RETURN_INT64(cmsketch_depth_histogram_c(sketch,
                                               DatumGetInt64(sketch->args[0])));
}

/*!
 * Greenplum "prefunc" to combine sketches from multiple machines
 */
PG_FUNCTION_INFO_V1(__cmsketch_merge);
Datum __cmsketch_merge(PG_FUNCTION_ARGS)
{
    bytea *     counterblob1 = PG_GETARG_BYTEA_P(0);
    bytea *     counterblob2 = PG_GETARG_BYTEA_P(1);
    cmtransval *transval1 = (cmtransval *)VARDATA(counterblob1);
    cmtransval *transval2 = (cmtransval *)VARDATA(counterblob2);
    cmtransval *newtrans;
    countmin *  sketches2 = (countmin *)
                            ((cmtransval *)(VARDATA(counterblob2)))->sketches;
    bytea *     newblob;
    countmin *  newsketches;
    uint32      i, j, k;
    int         sz;

    /* make sure they're initialized! */
    if (!CM_TRANSVAL_INITIALIZED(counterblob1)
        && !CM_TRANSVAL_INITIALIZED(counterblob2))
        /* if both are empty can return one of them */
        PG_RETURN_DATUM(PointerGetDatum(counterblob1));
    else if (!CM_TRANSVAL_INITIALIZED(counterblob1)) {
        elog(NOTICE, "typOid is %d", transval2->typOid);
        counterblob1 = cmsketch_init_transval(transval2->typOid);
        transval1 = (cmtransval *)VARDATA(counterblob1);
    }
    else if (!CM_TRANSVAL_INITIALIZED(counterblob2)) {
        elog(NOTICE, "typOid is %d", transval1->typOid);
        counterblob2 = cmsketch_init_transval(transval1->typOid);
        transval2 = (cmtransval *)VARDATA(counterblob2);
    }

    sz = VARSIZE(counterblob1);
    /* allocate a new transval as a copy of counterblob1 */
    newblob = (bytea *)palloc(sz);
    memcpy(newblob, counterblob1, sz);
    newtrans = (cmtransval *)(VARDATA(newblob));
    newsketches = (countmin *)(newtrans)->sketches;

    /* add in values from counterblob2 */
    for (i = 0; i < RANGES; i++)
        for (j = 0; j < DEPTH; j++)
            for (k = 0; k < NUMCOUNTERS; k++)
                newsketches[i][j][k] += sketches2[i][j][k];

    if (newtrans->nargs == -1) {
        /* transfer in the args from the other input */
        newtrans->nargs = transval2->nargs;
        for (i = 0; (int)i < transval2->nargs; i++)
            newtrans->args[i] = transval2->args[i];
    }

    PG_RETURN_DATUM(PointerGetDatum(newblob));
}



/*
 *******  Below are scalar methods to manipulate completed sketches.  ******
 */

/*!
 * match sketch type to scalar arg type
 */
#define CM_CHECKARG(sketch, arg_offset) \
    if (PG_ARGISNULL(arg_offset)) PG_RETURN_NULL(); \
    if (sketch->typOid != get_fn_expr_argtype(fcinfo->flinfo, arg_offset)) { \
        elog( \
            ERROR, \
            "sketch computed over type " INT64_FORMAT \
            "; argument %d over type " INT64_FORMAT ".", \
            (int64)sketch->typOid, \
            arg_offset, \
            (int64)get_fn_expr_argtype(fcinfo->flinfo, arg_offset)); \
        PG_RETURN_NULL(); \
    }

/*!
 * get the approximate count of objects with value arg
 * \param sketch a countmin sketch
 * \param arg the Datum we want to find the count of
 * \param funcOid the Postgres function that converts arg to a string
 */
int64 cmsketch_count_c(countmin sketch, Datum arg, Oid funcOid, Oid typOid)
{
    bytea *nhash;

    /* get the md5 hash of the argument. */
    nhash = sketch_md5_bytea(arg, typOid);
    return(cmsketch_count_md5_datum(sketch, nhash, funcOid));
}

int64 cmsketch_count_md5_datum(countmin sketch, bytea *md5_bytea, Oid funcOid)
{
    /* iterate through the sketches, finding the min counter associated with this hash */
    return(hash_counters_iterate(md5_bytea, sketch, INT64_MAX,
                                          &min_counter));
}

/*!
 * Compute count of a range by summing counts of its dyadic ranges
 * \param transval a countmin transition value
 * \param bot the bottom of the range (inclusive)
 * \param top the top of the range (inclusive)
 */
Datum cmsketch_rangecount_c(cmtransval *transval, int64 bot, int64 top)
{
    int64     cursum = 0;
    uint32    i;
    rangelist r;
    int4      dyad;
    int64     val;
    int64     countval;

    r.emptyoffset = 0;
    find_ranges(bot, top, &r);

    /*
     * find_ranges will not generate a span larger than 2^63-1, so
     * we only need to consider 2^63 or less.
     */
    for (i = 0; i < r.emptyoffset; i++) {
        /* What power of 2 is this range? */
        if (r.spans[i][0] == MIN_INT64 && r.spans[i][1] == -1) {
            /* special case to avoid overflow: full range left of 0 */
            dyad = 63;
            countval = -1;
        }
        else if (r.spans[i][0] == 0 && r.spans[i][1] == MAX_INT64) {
            /* special case to avoid overflow: full range right of 0 */
            dyad = 63;
            countval = 0;
        }
        else {
            int64 width = r.spans[i][1] - r.spans[i][0] + (int64)1;
            if (r.spans[i][0] == MIN_INT64)
                width++;
            if (r.spans[i][1] == MAX_INT64) {
                width++;
            }
            /* Divide min of range by 2^dyad and get count */
            dyad = safe_log2(width);
            countval = r.spans[i][0] >> dyad;
        }
        val = cmsketch_count_c(transval->sketches[dyad],
                               (Datum) countval,
                               transval->outFuncOid, transval->typOid);
        cursum += val;
    }
    PG_RETURN_DATUM(cursum);
}


/*!
 * convert an arbitrary range [bot-top] into a rangelist of dyadic ranges.
 * E.g. convert 14-48 into [[14-15], [16-31], [32-47], [48-48]]
 * To manage overflow issues, do not generate a range larger than 2^63-1!
 * \param bot the bottom of the range (inclusive)
 * \param top the top of the range (inclusive)
 * \param r the list of ranges to be returned
 */
void find_ranges(int64 bot, int64 top, rangelist *r)
{
    /* kick off the recursion at power RANGES-1 */
    find_ranges_internal(bot, top, RANGES-1, r);
}

/*!
 * find the ranges via recursive calls to this routine, pulling out smaller and
 * smaller powers of 2
 * To manage overflow issues, do not generate a range larger than 2^63-1!
 * \param bot the bottom of the range (inclusive)
 * \param top the top of the range (inclusive)
 * \param power the highest power to start with
 * \param r the rangelist to be returned
 */
void find_ranges_internal(int64 bot, int64 top, int power, rangelist *r)
{
    short  dyad;  /* a number between 0 and 63 */
    uint64 pow_dyad;
    uint64 width;

    /* Sanity check */
    if (top < bot || power < 0)
        return;

    if (top == bot) {
        /* base case of recursion, a range of the form [x-x]. */
        r->spans[r->emptyoffset][0] = r->spans[r->emptyoffset][1] = bot;
        ADVANCE_OFFSET(*r);
        return;
    }

    /*
     * a range that's 2^63 or larger will overflow int64's in the
     * code below, so recurse to the left and right by hand
     */
    if (top >= 0 && bot < 0) {
        /* don't even try to figure out how wide this range is, just split */
        find_ranges_internal(bot, -1, power-1, r);
        find_ranges_internal(0, top, power-1, r);
        return;
    }

    width = top - bot + (int64)1;

    /* account for the fact that MIN and MAX are 1 off the true power of 2 */
    if (top == MAX_INT64 || bot == MIN_INT64)
        width++;

    dyad = safe_log2(width);
    if (dyad > 62) {
        /* dangerously big, so split.  we know that we don't span 0. */
        int sign = (top < 0) ? -1 : 1;
        find_ranges_internal(bot, (((int64)1) << 62)*sign - 1, 62, r);
        find_ranges_internal((((int64)1) << 62)*sign, top, 62, r);
        return;
    }

    /*
     * if we get here, we have a range of size 2 or greater.
     * Find the largest dyadic range width in this range.
     */
    pow_dyad = ((uint64)1) << dyad;

    if ((bot == MIN_INT64) || (bot % pow_dyad == 0)) {
        /* our range is left-aligned on the dyad's min */
        r->spans[r->emptyoffset][0] = bot;
        if (top == MAX_INT64) {
            /*
             * special case: no need to recurse on right, and doing so
             * could overflow, so handle by hand
             */
            r->spans[r->emptyoffset][1] = MAX_INT64;
            ADVANCE_OFFSET(*r);
        }
        else {
            int64 newbot;
            /*
             * -1 on the next line because range to the right will
             * start at the power-of-2 boundary.
             */
            r->spans[r->emptyoffset][1] = (bot + pow_dyad - 1);
            if (bot == MIN_INT64) {
                /* account for fact that MIN_INT64 is 1 bigger than -2^63 */
                r->spans[r->emptyoffset][1]--;
            }
            newbot = r->spans[r->emptyoffset][1] + 1;
            ADVANCE_OFFSET(*r);
            /* recurse on right at finer grain */
            find_ranges_internal(newbot, top, power-1, r);
        }
    }

    else if (top == MAX_INT64 || ((top+1) % pow_dyad) == 0) {
        /* our range is right-aligned on the dyad's max. */
        r->spans[r->emptyoffset][1] = top;
        if (bot == MIN_INT64) {
            /*
             * special case: no need to recurse on left, and doing so
             * could overflow, so handle by hand
             */
            r->spans[r->emptyoffset][0] = MIN_INT64;
            ADVANCE_OFFSET(*r);
        }
        else {
            int64 newtop;
            r->spans[r->emptyoffset][0] = (top - pow_dyad + 1);
            if (top == MAX_INT64) {
                /* account for fact that MAX_INT64 is 1 smaller than 2^63 */
                r->spans[r->emptyoffset][0]++;
            }
            newtop = r->spans[r->emptyoffset][0] - 1;
            ADVANCE_OFFSET(*r);
            /* recurse on left at finer grain. */
            find_ranges_internal(bot, newtop, power-1, r);
        }
    }
    else {
        /* we straddle a power of 2 */
        int64 power_of_2 = pow_dyad*(top/pow_dyad);

        /* recurse on right at finer grain */
        find_ranges_internal(bot, power_of_2 - 1, power-1, r);
        /* recurse on left at finer grain */
        find_ranges_internal(power_of_2, top, power-1, r);
    }
}

/*!
 * find the approximate centile in a cm sketch by binary search in the domain of values
 * \param transval the current transition value
 * \param intcentile the centile to return
 * \param total the total count of items
 */
Datum cmsketch_centile_c(cmtransval *transval, int intcentile, int64 total)
{
    uint  i;
    int64 higuess,loguess,curguess,curcount;
    int64 centile_cnt;


    if (intcentile <= 0 || intcentile >= 100)
        elog(ERROR,
             "centiles must be between 1-99 inclusive, was %d",
             intcentile);


    centile_cnt = (int64)(total * (float8)intcentile/100.0);

    for (i = 0, loguess = MIN_INT64, higuess = MAX_INT64, curguess = 0;
         (i < INT64BITS - 1) && ((uint64)higuess-(uint64)loguess > 1);
         i++) {
        curcount = cmsketch_rangecount_c(transval, MIN_INT64, curguess);
        if (curcount == centile_cnt)
            break;
        if (curcount > centile_cnt) {
            /* overshot */
            higuess = curguess;
            curguess = loguess + (curguess - loguess) / 2;
        }
        else {
            /* undershot */
            loguess = curguess;
            curguess = higuess - (higuess - curguess) / 2;
        }
    }
    return(curguess);
}



PG_FUNCTION_INFO_V1(cmsketch_width_histogram);
/*!
 * scalar function taking a sketch, min, max, and number of buckets.
 * produces an equi-width histogram of that many buckets
 */
/* UDF wrapper.  All the interesting stuff is in __fmsketch_count_distinct_c */
Datum cmsketch_width_histogram(PG_FUNCTION_ARGS)
{
    bytea *     transblob = cmsketch_check_transval(fcinfo, false);
    cmtransval *transval = (cmtransval *)VARDATA(transblob);
    int64       min = PG_GETARG_INT64(1);
    int64       max = PG_GETARG_INT64(2);
    int         buckets = PG_GETARG_INT32(3);
    Datum       retval;

    CM_CHECKARG(transval, 1);
    CM_CHECKARG(transval, 2);
    if (PG_ARGISNULL(3)) PG_RETURN_NULL();

    retval = cmsketch_width_histogram_c(transval, min, max, buckets);
    PG_RETURN_DATUM(retval);
}

/*!
 * produces an equi-width histogram
 * \param transval the transition value of the agg
 * \param min minimum value in the sketch
 * \param max maximum value in the sketch
 * \param buckets the number of buckets in the histogram
 */
Datum cmsketch_width_histogram_c(cmtransval *transval,
                                 int64 min,
                                 int64 max,
                                 int64 buckets)
{
    int64      step;
    int        i;
    ArrayType *retval;
    int64      binlo, binhi, binval;
    Datum      histo[buckets][3];
    int        dims[2], lbs[2];
    int16      typlen;
    bool       typbyval;
    char       typalign;
    char       typdelim;
    Oid        typioparam;
    Oid        typiofunc;

    step = Max(trunc((float8)(max-min+1) / (float8)buckets), 1);
    for (i = 0; i < buckets; i++) {
        binlo = min + i*step;
        if (binlo > max) break;
        histo[i][0] = Int64GetDatum(binlo);
        binhi = (i == buckets-1) ? max : (min + (i+1)*step - 1);
        histo[i][1] = Int64GetDatum(binhi);
        binval =
            cmsketch_rangecount_c(transval,
                                  DatumGetInt64(histo[i][0]),
                                  DatumGetInt64(histo[i][1]));
        histo[i][2] = Int64GetDatum(binval);
    }


    /*
     * Get info about element type
     */
    get_type_io_data(transval->typOid, IOFunc_output,
                     &typlen, &typbyval,
                     &typalign, &typdelim,
                     &typioparam, &typiofunc);

    dims[0] = i;     /* may be less than requested buckets if too few values */
    dims[1] = 3;     /* lo, hi, and val */
    lbs[0] = 0;
    lbs[1] = 0;

    retval = construct_md_array((Datum *)histo,
                                NULL,
                                2,
                                dims,
                                lbs,
                                transval->typOid,
                                typlen,
                                typbyval,
                                typalign);
    PG_RETURN_ARRAYTYPE_P(retval);
}

/*!
 * produces an equi-depth histogram of a cm sketch
 * \param transval the transition value of the agg
 * \param buckets the number of buckets in the histogram
 */
Datum cmsketch_depth_histogram_c(cmtransval *transval, int64 buckets)
{
    int64      step;
    uint       i, nextbucket;
    ArrayType *retval;
    int64      binlo;
    Datum      histo[buckets][3];
    int        dims[2], lbs[2];
    int64      total = cmsketch_rangecount_c(transval, MIN_INT64, MAX_INT64);
    int16      typlen;
    bool       typbyval;
    char       typalign;
    char       typdelim;
    Oid        typioparam;
    Oid        typiofunc;

    step = Max(trunc(100 / (float8)buckets), 1);
    for (i = nextbucket = 0, binlo = MIN_INT64; i < buckets; i++) {
        uint64 centile;
        if (i < buckets - 1) {
            centile = cmsketch_centile_c(transval, (i+1)*step, total);
            if (i > 0 && centile <= histo[nextbucket-1][1])
                /* next centile is lower than previous; skip */
                continue;
            histo[nextbucket][1] = centile;
        }
        else {
            /* this is the top bucket */
            histo[nextbucket][1] = MAX_INT64;
        }
        histo[nextbucket][0] = Int64GetDatum(binlo);
        histo[nextbucket][2] = cmsketch_rangecount_c(transval,
                                                     DatumGetInt64(histo[
                                                                       nextbucket
                                                                   ][0]),
                                                     DatumGetInt64(histo[
                                                                       nextbucket
                                                                   ][1]));
        binlo = histo[nextbucket][1] + 1;
        nextbucket++;
    }

    /*
     * Get info about element type
     */
    get_type_io_data(transval->typOid, IOFunc_output,
                     &typlen, &typbyval,
                     &typalign, &typdelim,
                     &typioparam, &typiofunc);

    dims[0] = nextbucket;         /* may be less than requested buckets if too few values */
    dims[1] = 3;     /* lo, hi, and val */
    lbs[0] = 0;
    lbs[1] = 0;

    retval = construct_md_array((Datum *)histo,
                                NULL,
                                2,
                                dims,
                                lbs,
                                transval->typOid,
                                typlen,
                                typbyval,
                                typalign);
    PG_RETURN_ARRAYTYPE_P(retval);
}

/****** SUPPORT ROUTINES *******/
PG_FUNCTION_INFO_V1(cmsketch_dump);

/*!
 * dump sketch contents
 */
Datum cmsketch_dump(PG_FUNCTION_ARGS)
{
    bytea *   transblob = (bytea *)PG_GETARG_BYTEA_P(0);
    countmin *sketches;
    char *    newblob = (char *)palloc(10240);
    uint32    i, j, k, c;

    sketches = ((cmtransval *)VARDATA(transblob))->sketches;
    for (i=0, c=0; i < RANGES; i++)
        for (j=0; j < DEPTH; j++)
            for(k=0; k < NUMCOUNTERS; k++) {
                if (sketches[i][j][k] != 0)
                    c += sprintf(&newblob[c], "[(%d,%d,%d):" INT64_FORMAT
                                 "], ", i, j, k, sketches[i][j][k]);
                if (c > 10000) break;
            }
    newblob[c] = '\0';
    PG_RETURN_NULL();
}


/*!
 * for each row of the sketch, use the 16 bits starting at 2^i mod NUMCOUNTERS,
 * and invoke the lambda on those 16 bits (which may destructively modify counters).
 * \param hashval the MD5 hashed value that we take 16 bits at a time
 * \param sketch the cmsketch
 * \param initial the initialized return value
 * \param lambdaptr the function to invoke on each 16 bits
 */
int64 hash_counters_iterate(bytea *hashval,
                            countmin sketch, /* width is DEPTH*NUMCOUNTERS */
                            int64 initial,
                            int64 (*lambdaptr)(uint32,
                                               uint32,
                                               countmin,
                                               int64))
{
    uint32         i, col;
    char          *c;
    unsigned short twobytes;
    int64          retval = initial;

    /*
     * XXX WARNING: are there problems with unaligned access here?
     * XXX I was using copies rather than casting, which was inefficient,
     * XXX but I was hoping memmove would deal with unaligned access in a portable way.
     * XXX However the deref of 2 bytes seems to work OK.
     */
    for (i = 0, c = (char *)VARDATA(hashval); 
         i < DEPTH; 
         i++, c += 2) {
        // memmove((void *)&twobytes, c, 2);
        twobytes = *(unsigned short *)c;
        col = twobytes % NUMCOUNTERS;
        retval = (*lambdaptr)(i, col, sketch, retval);
    }
    return retval;
}

/*!
 * destructive increment lambda for hash_counters_iterate.
 * transval and return val not of particular interest here.
 * \param i which row to update
 * \param col which column to update
 * \param sketch the sketch
 * \param transval we don't need transval here, but its part of the
 * lambda interface for hash_counters_iterate
 */

int64 increment_counter(uint32 i,
                        uint32 col,
                        countmin sketch,
                        int64 transval)
{
    int64 oldval = sketch[i][col];
    if (sketch[i][col] == (INT64_MAX))
        elog(ERROR, "maximum count exceeded in sketch");
    sketch[i][col] = oldval + 1;

    /* return the incremented value, though unlikely anyone cares. */
    return oldval+1;
}

/*!
 * running minimum lambda for hash_counters_iterate
 * \param i which row to examine
 * \param col which column to examine
 * \param sketch the sketch
 * \param transval smallest counter so far
 * lambda interface for hash_counters_iterate
 */
int64 min_counter(uint32 i,
                  uint32 col,
                  countmin sketch,
                  int64 transval)
{
    int64 thisval = sketch[i][col];
    return (thisval < transval) ? thisval : transval;
}
