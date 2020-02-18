#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "3rd-party/frozen.h"

#include "3rd-party/slog.h"

#include "bigsi.h"
#include "errors.h"

/*****************************************************************************
 * bigsInit will initiate a BIGSI.
 * 
 * arguments:
 *      numBits                   - the number of bits per bloom filter
 *      numHashes                 - the number of hashes used per bloom filter
 *      dbDir                     - where to store the index files
 * 
 * returns:
 *      pointer to an initiated BIGSI
 * 
 * note:
 *      the user must free the returned BIGSI
 *      the user must check the returned BIGSI for NULL (failed to init)
 *      the index is not ready until the bigsIndex function is called
 */
bigsi_t *bigsInit(int numBits, int numHashes, const char *dbDir)
{
    assert(numBits > 0);
    assert(numHashes > 0);
    bigsi_t *newBIGSI;
    if ((newBIGSI = malloc(sizeof *newBIGSI)) != NULL)
    {
        newBIGSI->numBits = numBits;
        newBIGSI->numHashes = numHashes;
        newBIGSI->colourIterator = 0;
        newBIGSI->indexed = false;

        map_init(&newBIGSI->idChecker);
        newBIGSI->tmpBitVectors = NULL;
        newBIGSI->colourArray = NULL;

        newBIGSI->dbDirectory = dbDir;
        newBIGSI->bitvectors_dbp = NULL;
        newBIGSI->colours_dbp = NULL;
        newBIGSI->metadata_file = NULL;
        newBIGSI->bitvectors_db = NULL;
        newBIGSI->colours_db = NULL;
    }
    return newBIGSI;
}

/*****************************************************************************
 * bigsAdd will assign colours to seqIDs and add corresponding bloom filters
 * to the BIGSI.
 * 
 * arguments:
 *      bigsi                     - the BIGSI data structure
 *      id2bf                     - a map of the bloom filters to insert (seqID:bloomfilter)
 *      numEntries                - the number of sequences to be inserted (the number of bloom filters in id2bf)
 * 
 * returns:
 *      0 on success, -1 on error
 * 
 * note:
 *      the index is not ready until the bigsIndex function is called
 */
int bigsAdd(bigsi_t *bigsi, map_bloomfilter_t id2bf, int numEntries)
{

    // if this is the first insert, init the colour array and bit vector array
    if (!bigsi->colourArray)
    {
        if ((bigsi->colourArray = malloc(numEntries * sizeof(char **))) == NULL)
        {
            fprintf(stderr, "error: could not allocate colour array\n");
            return -1;
        }
        if ((bigsi->tmpBitVectors = malloc(numEntries * sizeof(bitvector_t **))) == NULL)
        {
            fprintf(stderr, "error: could not allocate bit vector array\n");
            return -1;
        }
    }

    // otherwise resize the existing arrays
    else
    {
        char **tmp = realloc(bigsi->colourArray, (bigsi->colourIterator + numEntries) * sizeof(char **));
        if (tmp)
        {
            bigsi->colourArray = tmp;
        }
        else
        {
            fprintf(stderr, "error: could not re-allocate colour array\n");
            return -1;
        }
        bitvector_t **tmp2 = realloc(bigsi->tmpBitVectors, (bigsi->colourIterator + numEntries) * sizeof(bitvector_t **));
        if (tmp2)
        {
            bigsi->tmpBitVectors = tmp2;
        }
        else
        {
            fprintf(stderr, "error: could not re-allocate bit vector array\n");
            return -1;
        }
    }

    // iterate over the input bloomfilter map, use the colourIterator to assign colours
    int inputMapCheck = 0;
    const char *seqID;
    map_iter_t iter = map_iter(&id2bf);
    while ((seqID = map_next(&id2bf, &iter)))
    {

        // if the seqID is already in BIGSI, return error
        if (map_get(&bigsi->idChecker, seqID) != NULL)
        {
            fprintf(stderr, "error: duplicate sequence ID can't be added to BIGSI (%s)\n", seqID);
            return -1;
        }

        // check the bloom filter is compatible and not empty
        bloomfilter_t *newBF = map_get(&id2bf, seqID);
        if (bvCount(newBF->bitvector) == 0)
        {
            fprintf(stderr, "error: empty bloom filter supplied to BIGSI for %s\n", seqID);
            return -1;
        }
        if ((newBF->numHashes != bigsi->numHashes) || (newBF->bitvector->capacity != bigsi->numBits))
        {
            fprintf(stderr, "error: bloom filter incompatible with BIGSI for %s\n", seqID);
            return -1;
        }

        // ADD 1 - get the bit vector from the bloom filter and add it to the BIGSI tmp array
        bigsi->tmpBitVectors[bigsi->colourIterator] = bvClone(newBF->bitvector);

        // ADD 2 - store the sequence ID to colour lookup for this bit vector
        map_set(&bigsi->idChecker, seqID, bigsi->colourIterator);

        // ADD 3 - store the colour to sequence ID lookup for this bit vector
        if ((bigsi->colourArray[bigsi->colourIterator] = malloc(strlen(seqID) + 1)) == NULL)
        {
            fprintf(stderr, "error: could not allocate colour memory for %s\n", seqID);
            return -1;
        }
        strcpy(bigsi->colourArray[bigsi->colourIterator], seqID);

        // increment the colour iterator
        bigsi->colourIterator++;
        if (bigsi->colourIterator == MAX_COLOURS)
        {
            fprintf(stderr, "error: maximum number of colours reached\n");
            return -1;
        }

        // increment the map checker
        inputMapCheck++;
    }

    // check the number of input bloom filters matched the number expected by the user, otherwise we'll have memory issues
    if (inputMapCheck != numEntries)
    {
        fprintf(stderr, "error: number bloom filters read did not match expected number (%u vs %u)\n", inputMapCheck, numEntries);
        return -1;
    }
    return 0;
}

/*****************************************************************************
 * bigsIndex will transform the input BIGSI bit vectors into the index,
 * prepare the databases and clean up the intermediatory data fields
 * 
 * arguments:
 *      bigsi                     - the BIGSI data structure
 * 
 * returns:
 *      0 on success, -1 on error
 */
int bigsIndex(bigsi_t *bigsi)
{

    // check there are some bit vectors inserted into the data structure
    if (bigsi->colourIterator < 1)
    {
        fprintf(stderr, "error: no bit vectors have been inserted into the BIGSI, nothing to index\n");
        return -1;
    }

    // check it hasn't already been indexed
    if (bigsi->indexed)
    {
        fprintf(stderr, "error: indexing has already been run on this BIGSI\n");
        return -1;
    }

    // TODO:
    // check filepath to dir for storage

    // initialize the Berkeley DBs
    setFilenames(bigsi);
    int ret;
    u_int32_t openFlags = DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN | DB_INIT_MPOOL | DB_THREAD;
    DBT key, data;
    ret = initDBs(bigsi, PROG_NAME, stderr, openFlags);
    if (ret)
    {
        fprintf(stderr, "error: could not create the Berkeley DBs (%u)\n", ret);
        return -1;
    }

    // iterate over all the BIGSI temporary bit vectors by bit index (equates to rows in matrix)
    // TODO: this is super inefficient - do better
    bitvector_t *newBV;
    for (int i = 0; i < bigsi->numBits; i++)
    {

        // create a new bit vector for this row
        newBV = bvInit(bigsi->colourIterator);
        if (newBV == NULL)
        {
            fprintf(stderr, "error: could not assign memory for new bit vector during BIGSI indexing\n");
            return -1;
        }

        // iterate over the bit vectors and check the bit index for the current row
        for (int colour = 0; colour < bigsi->colourIterator; colour++)
        {

            // check the bit vector
            if (bigsi->tmpBitVectors[colour] == NULL)
            {
                fprintf(stderr, "error: lost bit vector from BIGSI\n");
                return -1;
            }

            // check the bit at the required position (current BIGSI row) - skip if 0
            uint8_t bit = 0;
            if (bvGet(bigsi->tmpBitVectors[colour], i, &bit) != 0)
            {
                fprintf(stderr, "error: could not access bit at index position %u in bit vector (colour %u)\n", i, colour);
                return -1;
            }
            if (bit == 0)
            {
                continue;
            }
            // TODO: this check is unecessary as it shouldn't happen...
            bit = 0;
            if (bvGet(newBV, colour, &bit) != 0)
            {
                fprintf(stderr, "error: could not access bit at index position %u in bigsi index bit vector %u\n", colour, i);
                return -1;
            }
            if (bit == 1)
            {
                fprintf(stderr, "error: trying to set same bit twice in BIGSI bitvector %u for colour %u\n", i, colour);
                return -1;
            }

            // update the BIGSI index with this colour
            if (bvSet(newBV, colour, 1) != 0)
            {
                fprintf(stderr, "error: could not set bit\n");
                return -1;
            }
        }

        // add this bit vector to the database
        memset(&key, 0, sizeof(DBT));
        memset(&data, 0, sizeof(DBT));

        // use the current iterator as the key
        key.data = &i;
        key.size = sizeof(i);

        // use the bit new vector as the data
        data.data = newBV;
        data.size = (sizeof(bitvector_t) + (sizeof(uint8_t) * newBV->bufSize));

        // add it
        if (bigsi->bitvectors_dbp->put(bigsi->bitvectors_dbp, NULL, &key, &data, 0))
        {
            fprintf(stderr, "error: could not add bit vector to database (%u)\n", *(int *)key.data);
            return -1;
        }

        // free the original
        if (bvDestroy(newBV))
        {
            fprintf(stderr, "error: could not destroy temporary bit vector number %u\n", i);
            return -1;
        }
    }

    // now we've added all the bit vectors, do one last cycle over the colours and add them to the colour map with their corresponding sequence id
    for (int colour = 0; colour < bigsi->colourIterator; colour++)
    {
        memset(&key, 0, sizeof(DBT));
        memset(&data, 0, sizeof(DBT));
        key.data = &colour;
        key.size = sizeof(colour);
        data.data = bigsi->colourArray[colour];
        data.size = (u_int32_t)strlen(bigsi->colourArray[colour]) + 1;
        if (bigsi->colours_dbp->put(bigsi->colours_dbp, NULL, &key, &data, 0))
        {
            fprintf(stderr, "error: could not add colour to database (%u -> %s)\n", *(int *)key.data, (char *)data.data);
            return -1;
        }

        // while we're at it, free up everything we don't need for this colour
        free(bigsi->colourArray[colour]);
        bvDestroy(bigsi->tmpBitVectors[colour]);
    }

    // no longer need the duplicateChecker
    map_deinit(&bigsi->idChecker);
    bigsi->indexed = true;

    // check the DB
    return checkDB(bigsi);
}

/*****************************************************************************
 * bigsQuery will determine if any sequences in the BIGSI contain a query
 * k-mer (provided as one or more hash values).
 * 
 * arguments:
 *      bigsi                     - the BIGSI data structure
 *      hashValues                - the hash values to query
 *      len                       - the number of hash values being added
 *      result                    - an initialised bit vector to return the colours containing the query k-mer
 * 
 * returns:
 *      0 on success or an error code
 * 
 * note:
 *      it is the caller's responsibility to check and free the result
 *      it is the caller's responsibility to provide a pointer to an initiated, empty bit vector
 */
int bigsQuery(bigsi_t *bigsi, uint64_t *hashValues, unsigned int len, bitvector_t *result)
{
    // check a k-mer and result bit vector have been provided
    if (!hashValues || !result)
        return ERROR_NULL_ARGUMENT;

    // check the BIGSI is ready for querying
    if (!bigsi->indexed)
        return ERROR_BIGSI_UNINDEXED;

    // check the query hash vector matches the BIGSI settings
    if (len != bigsi->numHashes)
        return ERROR_BIGSI_HASH_MISMATCH;

    // check the result bit vector can hold all the colours
    if (result->capacity != bigsi->colourIterator)
        return ERROR_BIGSI_RESULT_MISMATCH;

    // set up the queries
    int ret = 0;
    DBT key, bv;
    memset(&key, 0, sizeof(key));
    memset(&bv, 0, sizeof(bv));

    unsigned int hv;
    key.ulen = sizeof(hv);
    key.size = sizeof(hv);
    key.flags = DB_DBT_USERMEM;
    bv.flags = DB_DBT_MALLOC;

    // collect the query hashes, get the correspoding bloom filter bits and retrieve the BIGSI rows
    for (int i = 0; i < len; i++)
    {

        // get the hash value location
        hv = *(hashValues + i) % bigsi->numBits;

        // setup the DB query
        key.data = &hv;

        slog(0, SLOG_LIVE, "query hashvale location: %i , size = %i, pointer to db = %i", *(int *)(key.data), key.size, bigsi->bitvectors_dbp);

        // query the DB for the corresponding bit vector in BIGSI for this hash value
        ret = bigsi->bitvectors_dbp->get(bigsi->bitvectors_dbp, 0, &key, &bv, 0);
        if (ret != 0)
        {
            slog(0, SLOG_LIVE, "bdb query failed: %i", ret);
            slog(0, SLOG_LIVE, "%s\n", db_strerror(ret));
            return -1;
        }

        // quick check to see if the bit vector is empty
        if (bvCount((bitvector_t *)bv.data) == 0)
        {
            return 0;
        }

        // update the result with this bit vector
        if (i == 0)
        {
            // first BIGSI hit can be the basis of the result, so just OR
            if (bvBOR(result, (bitvector_t *)bv.data, result) != 0)
                return ERROR_BIGSI_OR_FAIL;
        }
        else
        {

            // bitwise AND the current result with the new BIGSI hit
            if (bvBANDupdate(result, (bitvector_t *)bv.data) != 0)
                return ERROR_BIGSI_AND_FAIL;

            // if the current result is now empty, we can leave early
            if (bvCount(result) == 0)
            {
                return 0;
            }
        }
    }
    return 0;
}

/*****************************************************************************
 * bigsLookupColour will return the sequence ID stored for a query colour.
 * 
 * arguments:
 *      bigsi                     - the BIGSI data structure
 *      colour                    - the query colour
 *      result                    - a pointer to an uninitialised char* that will be used for the result
 * 
 * returns:
 *      0 on success, -1 on error
 * 
 * note:
 *      it is the caller's responsibility to check and free the result
 */

int bigsLookupColour(bigsi_t *bigsi, int colour, char **result)
{
    // check the BIGSI is ready for querying
    if (!bigsi->indexed)
    {
        fprintf(stderr, "error: need to run the bigsIndex function first\n");
        return -1;
    }

    // check query colour is in range
    if (colour > bigsi->colourIterator)
    {
        fprintf(stderr, "error: colour not present in BIGSI: %u\n", colour);
        return -1;
    }

    // check we can send the result
    if (!result)
    {
        fprintf(stderr, "error: no pointer provided for returning query results\n");
        return -1;
    }

    // set up the query
    int ret;
    DBT key, seqID;
    memset(&key, 0, sizeof(key));
    memset(&seqID, 0, sizeof(seqID));
    key.data = &colour;
    key.size = sizeof(colour);
    seqID.data = NULL;
    seqID.size = 0;
    key.flags = DB_DBT_USERMEM;
    seqID.flags = DB_DBT_MALLOC;

    // query the DB for the corresponding sequence ID in BIGSI for this colour
    if ((ret = bigsi->colours_dbp->get(bigsi->colours_dbp, NULL, &key, &seqID, 0)) != 0)
    {
        fprintf(stderr, "error: can't find colour in BIGSI (colour %d)\n", colour);
        return -1;
    }

    // set up the result
    *result = malloc((int)seqID.size);
    if (*result == NULL)
    {
        fprintf(stderr, "error: could not allocate memory for sequence ID retrieval\n");
        return -1;
    }
    strcpy(*result, ((char *)seqID.data));
    return 0;
}

/*****************************************************************************
 * bigsDestroy will clear an unindexed BIGSI and release the memory.
 * 
 * arguments:
 *      bigsi                     - the BIGSI data structure
 * 
 * returns:
 *      0 on success, -1 on error
 * 
 * note:
 *      if BIGSI is indexed, it will call the flush function instead
 */
int bigsDestroy(bigsi_t *bigsi)
{
    if (!bigsi)
    {
        fprintf(stderr, "error: no BIGSI was provided to bigsDestroy\n");
        return -1;
    }
    if (bigsi->indexed)
    {
        return bigsFlush(bigsi);
    }

    // if there has been a call to bigsAdd, there will be some freeing to do
    if (bigsi->colourIterator != 0)
    {
        for (int i = 0; i < bigsi->colourIterator; i++)
        {
            bvDestroy(bigsi->tmpBitVectors[i]);
            free(bigsi->colourArray[i]);
        }
        free(bigsi->tmpBitVectors);
        free(bigsi->colourArray);
        map_deinit(&bigsi->idChecker);
    }
    bigsi->numBits = 0;
    bigsi->numHashes = 0;
    bigsi->colourIterator = 0;
    free(bigsi);
    return 0;
}

/*****************************************************************************
 * bigsFlush will flush an indexed BIGSI to disk, then free the current
 * instance.
 * 
 * arguments:
 *      bigsi                     - the BIGSI data structure
 * 
 * returns:
 *      0 on success, -1 on error
 */
int bigsFlush(bigsi_t *bigsi)
{
    if (!bigsi->indexed)
    {
        fprintf(stderr, "error: can't flush an un-indexed BIGSI\n");
        return -1;
    }
    int status = json_fprintf(bigsi->metadata_file, "{ db_directory: %Q, metadata: %Q, bitvectors: %Q, colours: %Q, numBits: %d, numHashes: %d, colourIterator: %d }",
                              bigsi->dbDirectory,
                              bigsi->metadata_file,
                              bigsi->bitvectors_db,
                              bigsi->colours_db,
                              bigsi->numBits,
                              bigsi->numHashes,
                              bigsi->colourIterator);
    if (status < 0)
    {
        fprintf(stderr, "error: failed to write bigsi metadata to disk (%d)\n", status);
        return -1;
    }
    json_prettify_file(bigsi->metadata_file);

    // now the metadata is written, close down the dbs
    if (closeDBs(bigsi))
    {
        fprintf(stderr, "error: could not close the BIGSI databases\n");
        return -1;
    }
    free(bigsi->colours_db);
    free(bigsi->bitvectors_db);
    free(bigsi->metadata_file);
    free(bigsi);
    bigsi = NULL;
    return 0;
}

/*****************************************************************************
 * bigsLoad will load an indexed BIGSI from disk.
 * 
 * arguments:
 *      dbDir                  - what directory to load the BIGSI files from
 * 
 * returns:
 *      pointer to the loaded BIGSI
 * 
 * note:
 *      the user must free the returned BIGSI
 *      the user must check the returned BIGSI for NULL (failed to init)
 */
bigsi_t *bigsLoad(const char *dbDir)
{
    // get a bigsi with dummy values
    bigsi_t *bigsi = bigsInit(1, 1, dbDir);
    if (bigsi == NULL)
    {
        fprintf(stderr, "error: could not allocate memory for BIGSI\n");
        return NULL;
    }

    // setup the filenames
    setFilenames(bigsi);

    // check files exist in the provided dir
    if ((access(bigsi->metadata_file, R_OK | W_OK) == -1) || (access(bigsi->bitvectors_db, R_OK | W_OK) == -1) || (access(bigsi->colours_db, R_OK | W_OK) == -1))
    {
        fprintf(stderr, "error: could not access BIGSI database files in %s\n", bigsi->dbDirectory);
        return NULL;
    }

    // read the file into a buffer
    char *content = json_fread(bigsi->metadata_file);

    // scan the file content and populate the struct
    int status = json_scanf(content, strlen(content), "{ db_directory: %Q, metadata: %Q, bitvectors: %Q, colours: %Q, numBits: %d, numHashes: %d, colourIterator: %d }",
                            &bigsi->dbDirectory,
                            &bigsi->metadata_file,
                            &bigsi->bitvectors_db,
                            &bigsi->colours_db,
                            &bigsi->numBits,
                            &bigsi->numHashes,
                            &bigsi->colourIterator);

    // free the buffer
    free(content);

    // check for error in json scan (-1 == error, 0 == no elements found, >0 == elements parsed)
    if (status < 1)
    {
        fprintf(stderr, "error: could not scan metadata from json file\n");
        return NULL;
    }

    // load the dbs
    int ret;
    u_int32_t openFlags = DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN | DB_INIT_MPOOL | DB_THREAD;
    //openFlags = 0;
    if ((ret = initDBs(bigsi, PROG_NAME, stderr, openFlags)) != 0)
    {
        fprintf(stderr, "error: could not open the Berkeley DBs (%u)\n", ret);
        free(bigsi);
        return NULL;
    }
    bigsi->indexed = true;

    // check the DB look good
    if (checkDB(bigsi) != 0)
    {
        fprintf(stderr, "error: Berkeley DB check failed\n");
        free(bigsi);
        return NULL;
    }
    return bigsi;
}

// initDBs will initialise all the DBs in the BIGSI data structure
int initDBs(bigsi_t *bigsi, const char *program_name, FILE *error_file_pointer, u_int32_t openFlags)
{
    int ret;

    // open the colours database
    ret = openDB(&(bigsi->colours_dbp), bigsi->colours_db, program_name, error_file_pointer, openFlags);

    // error reporting is handled in openDB() so just return the return code here
    if (ret != 0)
    {
        return (ret);
    }

    // open the bit vectors database
    ret = openDB(&(bigsi->bitvectors_dbp), bigsi->bitvectors_db, program_name, error_file_pointer, openFlags);

    // error reporting is handled in openDB() so just return the return code here
    if (ret != 0)
    {
        return (ret);
    }
    return 0;
}

// closeDBs is a helper function to close the DBs in the BIGSI data structure
int closeDBs(bigsi_t *bigsi)
{
    int ret;

    // Note: closing a database automatically flushes its cached data to disk, so no sync is required here
    if (bigsi->colours_dbp != NULL)
    {
        ret = bigsi->colours_dbp->close(bigsi->colours_dbp, 0);
        if (ret != 0)
        {
            fprintf(stderr, "error: colour database close failed (%s)\n", db_strerror(ret));
        }
    }
    if (bigsi->bitvectors_dbp != NULL)
    {
        ret = bigsi->bitvectors_dbp->close(bigsi->bitvectors_dbp, 0);
        if (ret != 0)
        {
            fprintf(stderr, "error: bitvector database close failed (%s)\n", db_strerror(ret));
        }
    }
    return 0;
}

// openDB is a helper function for opening a BerkeleyDB
int openDB(DB **dbpp,                /* The DB handle that we are opening */
           const char *filename,     /* The file in which the db lives */
           const char *program_name, /* Name of the program calling this function */
           FILE *error_file_pointer, /* File where we want error messages sent */
           u_int32_t openFlags)      /* The flags to open the DB with */
{
    DB *dbp;
    int ret;

    // initialize the DB handle
    ret = db_create(&dbp, NULL, 0);
    if (ret != 0)
    {
        fprintf(error_file_pointer, "%s: %s\n", program_name, db_strerror(ret));
        return (ret);
    }

    // point to the memory malloc'd by db_create()
    *dbpp = dbp;

    // set up error handling for this database
    dbp->set_errfile(dbp, error_file_pointer);
    dbp->set_errpfx(dbp, program_name);

    // open the database
    ret = dbp->open(dbp,              /* Pointer to the database */
                    NULL,             /* Txn pointer */
                    filename,         /* File name */
                    NULL,             /* Logical db name (unneeded) */
                    BERKELEY_DB_TYPE, /* Database type (using btree) */
                    openFlags,        /* Open flags */
                    0);               /* File mode. Using defaults */
    if (ret != 0)
    {
        dbp->err(dbp, ret, "failed to open '%s'", filename);
        return (ret);
    }
    return 0;
}

// setFilenames is a helper function to allocate and set the filenames for the index files
void setFilenames(bigsi_t *bigsi)
{
    size_t size;

    // set filename for the metadata
    size = strlen(bigsi->dbDirectory) + strlen(BIGSI_METADATA_FILENAME) + 2;
    bigsi->metadata_file = malloc(size);
    snprintf(bigsi->metadata_file, size, "%s/%s", bigsi->dbDirectory, BIGSI_METADATA_FILENAME);

    // set filename for the bitvectors database
    size = strlen(bigsi->dbDirectory) + strlen(BITVECTORS_DB_FILENAME) + 2;
    bigsi->bitvectors_db = malloc(size);
    snprintf(bigsi->bitvectors_db, size, "%s/%s", bigsi->dbDirectory, BITVECTORS_DB_FILENAME);

    // set filename for the colours database
    size = strlen(bigsi->dbDirectory) + strlen(COLOURS_DB_FILENAME) + 2;
    bigsi->colours_db = malloc(size);
    snprintf(bigsi->colours_db, size, "%s/%s", bigsi->dbDirectory, COLOURS_DB_FILENAME);
}

// checkDB is a helper function to check that the berkeleyDB is accessible
int checkDB(bigsi_t *bigsi)
{
    assert(bigsi != NULL);

    // check that the Berkeley DB works by querying the last few rows
    bitvector_t *result = bvInit(bigsi->colourIterator);
    assert(result != NULL);
    int err = 0;
    uint64_t *hvs = calloc(bigsi->numHashes, sizeof(uint64_t));
    for (int i = 0; i < bigsi->numHashes; i++)
        *(hvs + i) = bigsi->numBits - 1 - i;
    err = bigsQuery(bigsi, hvs, bigsi->numHashes, result);
    free(hvs);
    free(result);
    if (err != 0)
    {
        fprintf(stderr, "could not query the BIGSI for known entries: %s\n", printError(err));
        return -1;
    }
    return 0;
}