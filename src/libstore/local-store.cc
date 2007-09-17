#include "references.hh"
#include "config.h"
#include "local-store.hh"
#include "util.hh"
#include "globals.hh"
#include "db.hh"
#include "archive.hh"
#include "pathlocks.hh"
#include "aterm.hh"
#include "derivations-ast.hh"
#include "worker-protocol.hh"

#include "derivations.hh"
#include "misc.hh"

#include "store-state.hh"

#include <iostream>
#include <algorithm>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <time.h>

namespace nix {

    
/* Nix database. */
static Database nixDB;


/* Database tables. */

/* dbValidPaths :: Path -> ()

   The existence of a key $p$ indicates that path $p$ is valid (that
   is, produced by a succesful build). */
static TableId dbValidPaths = 0;

/* dbValidStatePaths :: Path -> ()

   The existence of a key $p$ indicates that state path $p$ is valid (that
   is, produced by a succesful build). */
static TableId dbValidStatePaths = 0;

/* dbReferences :: Path -> [Path]

   This table lists the outgoing file system references for each
   output path that has been built by a Nix derivation.  These are
   found by scanning the path for the hash components of input
   paths. */
static TableId dbComponentComponentReferences = 0;
static TableId dbComponentStateReferences = 0;

/* dbStateReferences :: Path -> [Path]

   This table lists the outgoing file system state references for each
   output path that has been built by a Nix derivation.  These are
   found by scanning the path for the hash components of input
   paths. */
static TableId dbStateComponentReferences = 0;
static TableId dbStateStateReferences = 0;

/* dbSolidStateReferences :: Path -> [Path] 
 * 
 * This is used to store references that are ALWAYS detected while scanning 
 * the store path, this is to include components that cannont be configured
 * to put their state in /nix/state. We then use this workaround:
 * mozilla firefox has state in 
 * ~/.mozilla/firefox --symlinked_to--> /nix/state/...HASH.....-firefox/ 
 * 
 * Now the component firefox in the store does now detect the HASH since it
 * is not in there, so we store the state path in this table which causes a
 * scan of the firefox-store path to always return the firefox state path. 
 * 
 */
static TableId dbSolidStateReferences = 0;

/* dbSubstitutes :: Path -> [[Path]]

   Each pair $(p, subs)$ tells Nix that it can use any of the
   substitutes in $subs$ to build path $p$.  Each substitute defines a
   command-line invocation of a program (i.e., the first list element
   is the full path to the program, the remaining elements are
   arguments).

   The main purpose of this is for distributed caching of derivates.
   One system can compute a derivate and put it on a website (as a Nix
   archive), for instance, and then another system can register a
   substitute for that derivate.  The substitute in this case might be
   a Nix derivation that fetches the Nix archive.
*/
static TableId dbSubstitutes = 0;

/* dbDerivers :: Path -> [Path]

   This table lists the derivation used to build a path.  There can
   only be multiple such paths for fixed-output derivations (i.e.,
   derivations specifying an expected hash). */
static TableId dbDerivers = 0;

/* dbStateCounters :: StatePath -> Int
 * This table lists the state path sub folders that are of type interval.
 *
 * Some folders/files need only to be committed every 10'th run, so
 * we store the counting in this table 
 */
static TableId dbStateCounters = 0;

/* dbStateInfo :: Path -> 
 * 
 * This table lists all the store components, that are state-store components
 * meaning that this component has a /nix/state/ entry to store its state
 * and thus this component (should) have a reference to that state path  
 * 
 * TODO the value is now empty but we could store the entire DRV in here in the future
 */
static TableId dbStateInfo = 0;

/* dbStateRevisions :: StatePath -> [StatePath]
 * 
 * This table stores the revisions of state components and its dependecies
 * on other state components. A revsion has a number of statepaths at a 
 * certain a timestamp.
 * 
 * For example, a state path A at revision 1 depends on its own statepath at
 * a ceratain timstamp and 1 other statepath at a certain timstamp:
 *  
 * /nix/state/HASH-A-1.0-test-KEY-1
 * -->
 * [ /nix/state/HASH-A-1.0-test-KEY-1186135321, /nix/state/HASH-B-1.0-test-KEY-1186135321 ]
 *
 */
static TableId dbStateRevisions = 0;

/*
 * A additional table to store comments for revisions 
 */
static TableId dbStateRevisionsComments = 0;

/* dbStateSnapshots :: StatePath -> RevisionClosure
 * 
 * This table stores the snapshot numbers the sub state files/folders
 * at a certain timestamp. These snapshot numbers are just timestamps
 * produced by ext3cow 
 * 
 * /nix/state/HASH-A-1.0-test-KEY-1185473750
 * -->
 * [ 1185473750, 00118547375 ]
 * 
 * The timestamps are ordered based on the path of the subfolder !!
 * 
 * So if a has:
 * 
 * /nix/state/....A../log
 * /nix/state/....A../cache
 * 
 * then ./cache has TS 1185473750
 * and ./log has TS 00118547375
 *  
 */
static TableId dbStateSnapshots = 0;

/* dbSharedState :: Path -> Path
 * 
 * This table links each statepath to a list of epoch numbers.
 * These numbers represent the timestamps of the sub-state folders 
 * when they are snapshotted.
 * 
 *   
 */
static TableId dbSharedState = 0;

bool Substitute::operator == (const Substitute & sub) const
{
    return program == sub.program
        && args == sub.args;
}


static void upgradeStore07();
static void upgradeStore09();


void checkStoreNotSymlink()
{
    if (getEnv("NIX_IGNORE_SYMLINK_STORE") == "1") return;
    Path path = nixStore;
    struct stat st;
    while (path != "/") {
        if (lstat(path.c_str(), &st))
            throw SysError(format("getting status of `%1%'") % path);
        if (S_ISLNK(st.st_mode))
            throw Error(format(
                "the path `%1%' is a symlink; "
                "this is not allowed for the Nix store and its parent directories")
                % path);
        path = dirOf(path);
    }
}


LocalStore::LocalStore(bool reserveSpace)
{
    if (readOnlyMode) return;

    checkStoreNotSymlink();

    try {
        Path reservedPath = nixDBPath + "/reserved";
        string s = querySetting("gc-reserved-space", "");
        int reservedSize;
        if (!string2Int(s, reservedSize)) reservedSize = 1024 * 1024;
        if (reserveSpace) {
            struct stat st;
            if (stat(reservedPath.c_str(), &st) == -1 ||
                st.st_size != reservedSize)
                writeFile(reservedPath, string(reservedSize, 'X'));
        }
        else
            deletePath(reservedPath);
    } catch (SysError & e) { /* don't care about errors */
    }

    try {
        nixDB.open(nixDBPath);
    } catch (DbNoPermission & e) {
        printMsg(lvlTalkative, "cannot access Nix database; continuing anyway");
        readOnlyMode = true;
        return;
    }
    dbValidPaths = nixDB.openTable("validpaths");
    dbValidStatePaths = nixDB.openTable("validpaths_state");
    dbSubstitutes = nixDB.openTable("substitutes");
    dbDerivers = nixDB.openTable("derivers");

	dbStateInfo = nixDB.openTable("stateinfo");
    dbStateCounters = nixDB.openTable("statecounters");
	dbComponentComponentReferences = nixDB.openTable("references");	/* c_c */
	dbComponentStateReferences = nixDB.openTable("references_c_s");
	dbStateComponentReferences = nixDB.openTable("references_s_c");
	dbStateStateReferences = nixDB.openTable("references_s_s");
	dbStateRevisions = nixDB.openTable("staterevisions");
	dbStateRevisionsComments = nixDB.openTable("staterevisions_comments");
	dbStateSnapshots = nixDB.openTable("stateSnapshots");
	dbSharedState = nixDB.openTable("sharedState");
	dbSolidStateReferences = nixDB.openTable("references_solid_c_s");	/* The contents of this table is included in references_c_s */
	
    int curSchema = 0;
    Path schemaFN = nixDBPath + "/schema";
    if (pathExists(schemaFN)) {
        string s = readFile(schemaFN);
        if (!string2Int(s, curSchema))
            throw Error(format("`%1%' is corrupt") % schemaFN);
    }

    if (curSchema > nixSchemaVersion && curSchema != 4)					//TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!!! MAJOR HACK, I SHOULD MERGE WITH THE TRUNK
        throw Error(format("current Nix store schema is version %1%, but I only support %2%")
            % curSchema % nixSchemaVersion);

    if (curSchema < nixSchemaVersion && curSchema != 4) {				//TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!!! MAJOR HACK, I SHOULD MERGE WITH THE TRUNK
        if (curSchema <= 1)
            upgradeStore07();
        if (curSchema == 2)
            upgradeStore09();
        writeFile(schemaFN, (format("%1%") % nixSchemaVersion).str());
    }
}


LocalStore::~LocalStore()
{
    /* If the database isn't open, this is a NOP. */
    nixDB.close();
}


void createStoreTransaction(Transaction & txn)
{
    Transaction txn2(nixDB);
    txn2.moveTo(txn);
}


void copyPath(const Path & src, const Path & dst, PathFilter & filter)
{
    debug(format("copying `%1%' to `%2%'") % src % dst);

    /* Dump an archive of the path `src' into a string buffer, then
       restore the archive to `dst'.  This is not a very good method
       for very large paths, but `copyPath' is mainly used for small
       files. */ 

    StringSink sink;
    dumpPath(src, sink, filter);

    StringSource source(sink.s);
    restorePath(dst, source);
}


static void _canonicalisePathMetaData(const Path & path)
{
    checkInterrupt();

    struct stat st;
    if (lstat(path.c_str(), &st))
	throw SysError(format("getting attributes of path `%1%'") % path);

    /* Change ownership to the current uid.  If its a symlink, use
       lchown if available, otherwise don't bother.  Wrong ownership
       of a symlink doesn't matter, since the owning user can't change
       the symlink and can't delete it because the directory is not
       writable.  The only exception is top-level paths in the Nix
       store (since that directory is group-writable for the Nix build
       users group); we check for this case below. */
    if (st.st_uid != geteuid()) {
#if HAVE_LCHOWN
        if (lchown(path.c_str(), geteuid(), (gid_t) -1) == -1)
#else
        if (!S_ISLNK(st.st_mode) &&
            chown(path.c_str(), geteuid(), (gid_t) -1) == -1)
#endif
            throw SysError(format("changing owner of `%1%' to %2%")
                % path % geteuid());
    }
    
    if (!S_ISLNK(st.st_mode)) {

        /* Mask out all type related bits. */
        mode_t mode = st.st_mode & ~S_IFMT;
        
        if (mode != 0444 && mode != 0555) {
            mode = (st.st_mode & S_IFMT)
                 | 0444
                 | (st.st_mode & S_IXUSR ? 0111 : 0);
            if (chmod(path.c_str(), mode) == -1)
                throw SysError(format("changing mode of `%1%' to %2$o") % path % mode);
        }

        if (st.st_mtime != 0) {
            struct utimbuf utimbuf;
            utimbuf.actime = st.st_atime;
            utimbuf.modtime = 0;
            if (utime(path.c_str(), &utimbuf) == -1) 
                throw SysError(format("changing modification time of `%1%'") % path);
        }

    }

    if (S_ISDIR(st.st_mode)) {
        Strings names = readDirectory(path);
	for (Strings::iterator i = names.begin(); i != names.end(); ++i)
	    _canonicalisePathMetaData(path + "/" + *i);
    }
}


void canonicalisePathMetaData(const Path & path)
{
    _canonicalisePathMetaData(path);

    /* On platforms that don't have lchown(), the top-level path can't
       be a symlink, since we can't change its ownership. */
    struct stat st;
    if (lstat(path.c_str(), &st))
	throw SysError(format("getting attributes of path `%1%'") % path);

    if (st.st_uid != geteuid()) {
        assert(S_ISLNK(st.st_mode));
        throw Error(format("wrong ownership of top-level store path `%1%'") % path);
    }
}


bool isValidPathTxn(const Transaction & txn, const Path & path)
{
    string s;
    return nixDB.queryString(txn, dbValidPaths, path, s);
}

bool LocalStore::isValidPath(const Path & path)
{
    return isValidPathTxn(noTxn, path);
}

bool isValidStatePathTxn(const Transaction & txn, const Path & path)
{
    string s;
    return nixDB.queryString(txn, dbValidStatePaths, path, s);
}

bool LocalStore::isValidStatePath(const Path & path)
{
    return isValidStatePathTxn(noTxn, path);
}

bool isValidComponentOrStatePathTxn(const Transaction & txn, const Path & path)
{
    return (isValidPathTxn(txn, path) || isValidStatePathTxn(txn, path)); 
}

bool LocalStore::isValidComponentOrStatePath(const Path & path)
{
    return isValidComponentOrStatePathTxn(noTxn, path);
}


static Substitutes readSubstitutes(const Transaction & txn,
    const Path & srcPath);


static bool isRealisablePath(const Transaction & txn, const Path & path)
{
    return isValidPathTxn(txn, path) || readSubstitutes(txn, path).size() > 0;
}

static bool isRealisableStatePath(const Transaction & txn, const Path & path)
{
    return isValidStatePathTxn(txn, path) || readSubstitutes(txn, path).size() > 0;
}

static bool isRealisableComponentOrStatePath(const Transaction & txn, const Path & path)
{
    return isValidComponentOrStatePathTxn(txn, path) || readSubstitutes(txn, path).size() > 0;					//TODO State paths are not yet in substitutes !!!!!!!!!!!!!! ??
}

/*
static string addPrefix(const string & prefix, const string & s)
{
    return prefix + string(1, (char) 0) + s;
}

static string stripPrefix(const string & prefix, const string & s)
{
    if (s.size() <= prefix.size() ||
        string(s, 0, prefix.size()) != prefix ||
        s[prefix.size()] != 0)
        throw Error(format("string `%1%' is missing prefix `%2%'")
            % s % prefix);
    return string(s, prefix.size() + 1);
}
*/


void setReferences(const Transaction & txn, const Path & store_or_statePath,
    const PathSet & references, const PathSet & stateReferences, const unsigned int revision)
{
    /* For unrealisable paths, we can only clear the references. */
    if (references.size() > 0 && !isRealisableComponentOrStatePath(txn, store_or_statePath))
        throw Error(format("cannot set references for path `%1%' which is invalid and has no substitutes") % store_or_statePath);
	
	
	/*
	for (PathSet::iterator i = references.begin(); i != references.end(); ++i)
    	printMsg(lvlError, format("'%2%' has references: %1%") % *i % store_or_statePath);
    for (PathSet::iterator i = stateReferences.begin(); i != stateReferences.end(); ++i)
    	printMsg(lvlError, format("'%2%' has stateReferences: %1%") % *i % store_or_statePath);
	*/
	
    if(isRealisablePath(txn, store_or_statePath))
    {
		printMsg(lvlError, format("Setting references for storepath '%1%'") % store_or_statePath);
		
		//Just overwrite the old references, since there is oly 1 revision of a storePath
		
		Paths oldReferences_c_c;
    	Paths oldReferences_c_s;
    	nixDB.queryStrings(txn, dbComponentComponentReferences, store_or_statePath, oldReferences_c_c);
		nixDB.queryStrings(txn, dbComponentStateReferences, store_or_statePath, oldReferences_c_s);

    	PathSet oldReferences = PathSet(oldReferences_c_c.begin(), oldReferences_c_c.end());
    	PathSet oldStateReferences = PathSet(oldReferences_c_s.begin(), oldReferences_c_s.end());
    	if (oldReferences == references && oldStateReferences == stateReferences) return; 
    	
    	nixDB.setStrings(txn, dbComponentComponentReferences, store_or_statePath, Paths(references.begin(), references.end()));
		nixDB.setStrings(txn, dbComponentStateReferences, store_or_statePath, Paths(stateReferences.begin(), stateReferences.end()));
   		
    }
    else if(isRealisableStatePath(txn, store_or_statePath))
    {
		
		printMsg(lvlError, format("Setting references for statepath '%1%' (revision:%2%)") % store_or_statePath % unsignedInt2String(revision));
		
		//Write references to a special revision (since there are multiple revisions of a statePath)

		//query the references of revision (0 is query the latest references)
		Paths oldStateReferences_s_c;
		Paths oldStateReferences_s_s;
		nixDB.queryStateReferences(txn, dbStateComponentReferences, dbStateRevisions, store_or_statePath, oldStateReferences_s_c, revision); 
		nixDB.queryStateReferences(txn, dbStateStateReferences, dbStateRevisions, store_or_statePath, oldStateReferences_s_s, revision);

		PathSet oldReferences = PathSet(oldStateReferences_s_c.begin(), oldStateReferences_s_c.end());
    	PathSet oldStateReferences = PathSet(oldStateReferences_s_s.begin(), oldStateReferences_s_s.end());

		//set the references of revision (0 insert as a new timestamp)
    	nixDB.setStateReferences(txn, dbStateComponentReferences, dbStateRevisions, store_or_statePath, Paths(references.begin(), references.end()), revision);
		nixDB.setStateReferences(txn, dbStateStateReferences, dbStateRevisions, store_or_statePath, Paths(stateReferences.begin(), stateReferences.end()), revision);
    }
    else
    	throw Error(format("Path '%1%' is not a valid component or state path") % store_or_statePath);
}

void queryXReferencesTxn(const Transaction & txn, const Path & store_or_statePath, PathSet & references, const bool component_or_state, const unsigned int revision, const unsigned int timestamp)
{
    Paths references2;
    TableId table1;
    TableId table2;
    
    if(component_or_state){
    	table1 = dbComponentComponentReferences;
    	table2 = dbStateComponentReferences;
    }
    else{
    	table1 = dbComponentStateReferences;
    	table2 = dbStateStateReferences;
    }    	
    
    if(isRealisablePath(txn, store_or_statePath))
    	nixDB.queryStrings(txn, table1, store_or_statePath, references2);
    else if(isRealisableStatePath(txn, store_or_statePath)){
    	Path statePath_ns = toNonSharedPathTxn(txn, store_or_statePath);	    //Lookup its where it points to if its shared
    	nixDB.queryStateReferences(txn, table2, dbStateRevisions, statePath_ns, references2, revision, timestamp);
    }
    else
    	throw Error(format("Path '%1%' is not a valid component or state path") % store_or_statePath);
    
    references.insert(references2.begin(), references2.end());
}

void LocalStore::queryReferences(const Path & storePath, PathSet & references, const unsigned int revision)
{
    nix::queryXReferencesTxn(noTxn, storePath, references, revision, true);
}

void LocalStore::queryStateReferences(const Path & componentOrstatePath, PathSet & stateReferences, const unsigned int revision)
{
    nix::queryXReferencesTxn(noTxn, componentOrstatePath, stateReferences, revision, false);
}

static PathSet getXReferrers(const Transaction & txn, const Path & store_or_statePath, const bool component_or_state, const unsigned int revision)
{
    TableId table = 0;
    Path path;
    
    if( !isValidPathTxn(txn, store_or_statePath) && !isValidStatePathTxn(txn, store_or_statePath) )
    	throw Error(format("Path '%1%' is not a valid component or state path") % store_or_statePath);
    
    //NO TS
    if(component_or_state){	//we need component references
		if(isValidPathTxn(txn, store_or_statePath)){
			path = store_or_statePath;
			table = dbComponentComponentReferences;	
		}
		else if(isValidStatePathTxn(txn, store_or_statePath)){
			path = toNonSharedPathTxn(txn, store_or_statePath);	    //Lookup its where it points to if its shared
			table = dbComponentStateReferences;
    	}	
						
		//printMsg(lvlError, format("we need component references: '%1%' '%2%' '%3%'") % path % store_or_statePath % table);
	
		//check which key refers to our 'path' (we dont have to deal with timestamps since componentpaths are immutable)	
		Strings keys;
		nixDB.enumTable(txn, table, keys);
	    PathSet referrers;
		for (Strings::const_iterator i = keys.begin(); i != keys.end(); ++i){
			
			Strings references;
			nixDB.queryStrings(txn, table, *i, references);
			for (Strings::iterator j = references.begin(); j != references.end(); ++j){ 
				if(*j == path)
					referrers.insert(*i);	//we found a referrer
			}
		}
		
		return referrers;
    }
    //TS
    else{	//we need state references
	   	if(isValidPathTxn(txn, store_or_statePath)){
			path = store_or_statePath;
			table = dbStateComponentReferences;	
	   	}
		else if(isValidStatePathTxn(txn, store_or_statePath)){
			path = toNonSharedPathTxn(txn, store_or_statePath);	    //Lookup its where it points to if its shared
			table = dbStateStateReferences;
		}
			
		//printMsg(lvlError, format("we need state references: '%1%' '%2%' '%3%'") % path % store_or_statePath % table);
		
		//Now in references of ALL referrers, (possibly lookup their latest TS based on revision)
		unsigned int timestamp;
		if(revision != 0){
    		bool succeed = nixDB.revisionToTimeStamp(txn, dbStateRevisions, path, revision, timestamp);
	    	if(!succeed)
	    		throw Error(format("Getreferrers cannot find timestamp for revision: '%1%'") % revision);
		}
	    
	    Strings keys;
		nixDB.enumTable(txn, table, keys);
		map<string, unsigned int> latest;
		for (Strings::const_iterator i = keys.begin(); i != keys.end(); ++i){
			Path getStatePath;
			unsigned int getRevision;
			nixDB.splitDBKey(*i, getStatePath, getRevision);
		
			if(latest[getStatePath]	== 0) 						//either it is unset
				latest[getStatePath] = getRevision;
			else if(latest[getStatePath] < getRevision){ 			
				if(revision != 0 && getRevision <= timestamp)	//or it is greater, but not greater then the timestamp			//TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!! WERE COMPARING A REVISION TO A TIMESTAMP ???
					latest[getStatePath] = getRevision;
				else											//we need the latest so greater is good
					latest[getStatePath] = getRevision;
			}
		}
	    
	    //now check if they refer to 'path' (if you cant find it, then they dont referr to it)
	    PathSet referrers;
		for (map<string, unsigned int>::const_iterator i = latest.begin(); i != latest.end(); ++i){

			//printMsg(lvlError, format("AAAAA '%1%'") % (*i).first);
			
			Strings references;
			nixDB.queryStrings(txn, table, nixDB.mergeToDBKey((*i).first, (*i).second), references);
			for (Strings::iterator j = references.begin(); j != references.end(); ++j){ 
				//printMsg(lvlError, format("TEST: '%1%' has ref '%2%' check with '%3%'") % (*i).first % *j % path);
				if(*j == path)
					referrers.insert((*i).first);	//we found a referrer
			}
		}
		return referrers;	
    }
}

static PathSet getReferrersTxn(const Transaction & txn, const Path & store_or_statePath, const unsigned int revision)
{
	return getXReferrers(txn, store_or_statePath, true, revision);
}

static PathSet getStateReferrersTxn(const Transaction & txn, const Path & store_or_statePath, const unsigned int revision)
{
	return getXReferrers(txn, store_or_statePath, false, revision);
}

void queryReferrersTxn(const Transaction & txn,
    const Path & storePath, PathSet & referrers, const unsigned int revision)
{
    if (!isRealisableComponentOrStatePath(txn, storePath))
		throw Error(format("path `%1%' is not valid") % storePath);
    PathSet referrers2 = getReferrersTxn(txn, storePath, revision);
    referrers.insert(referrers2.begin(), referrers2.end());
}

void LocalStore::queryReferrers(const Path & storePath,
    PathSet & referrers, const unsigned int revision)
{
    nix::queryReferrersTxn(noTxn, storePath, referrers, revision);
}

void queryStateReferrersTxn(const Transaction & txn, const Path & storePath, PathSet & stateReferrers, const unsigned int revision)
{
    if (!isRealisableComponentOrStatePath(txn, storePath))
        throw Error(format("path `%1%' is not valid") % storePath);
    PathSet stateReferrers2 = getStateReferrersTxn(txn, storePath, revision);
    stateReferrers.insert(stateReferrers2.begin(), stateReferrers2.end());
}

void LocalStore::queryStateReferrers(const Path & storePath, PathSet & stateReferrers, const unsigned int revision)
{
    nix::queryStateReferrersTxn(noTxn, storePath, stateReferrers, revision);
}

void setDeriver(const Transaction & txn, const Path & storePath, const Path & deriver)
{
    assertStorePath(storePath);
    if (deriver == "") return;
    assertStorePath(deriver);
    
    if (!isRealisablePath(txn, storePath))
        throw Error(format("path `%1%' is not valid") % storePath);
	
    if (isStateDrvPathTxn(txn, deriver)){								//Redirect if its a state component					
    	addStateDeriver(txn, storePath, deriver);
	}		
    else{
	    nixDB.setString(txn, dbDerivers, storePath, deriver);
    }
}

/* Private function only used by addStateDeriver
 * Merges a new derivation into a list of derivations, this function takes username and statepath 
 * into account. This function is used to update derivations that have only changed in their sub state
 * paths that need to be versioned for example. We assume newdrv is the newest.
 */
PathSet mergeNewDerivationIntoListTxn(const Transaction & txn, const Path & storepath, const Path & newdrv, const PathSet drvs, bool deleteDrvs)
{
	PathSet newdrvs;

	Derivation drv = derivationFromPathTxn(txn, newdrv);
	string identifier = drv.stateOutputs.find("state")->second.stateIdentifier;
	string user = drv.stateOutputs.find("state")->second.username;
	
	for (PathSet::iterator i = drvs.begin(); i != drvs.end(); ++i)	//Check if we need to remove old drvs
    {
    	Path drv = *i;
    	Derivation getdrv = derivationFromPathTxn(txn, drv);
    	string getIdentifier = getdrv.stateOutputs.find("state")->second.stateIdentifier;
		string getUser = getdrv.stateOutputs.find("state")->second.username;
		
		if(identifier == getIdentifier && getUser == user)			//only insert if it doenst already exist
		{	
			//We also check if it's NOT exactly the same drvpath
			if(drv != newdrv && deleteDrvs){
				printMsg(lvlTalkative, format("Deleting decrepated state derivation: %1% with identifier %2% and user %3%") % drv % identifier % user);
				deletePath(drv);			//Deletes the DRV from DISK!
			}
		}	
		else
			newdrvs.insert(drv);
    }
    
    newdrvs.insert(newdrv);
    return newdrvs;
}

void addStateDeriver(const Transaction & txn, const Path & storePath, const Path & deriver)
{
	assertStorePath(storePath);
	if (deriver == "") 
		return;
	assertStorePath(deriver);
	
    if (!isRealisablePath(txn, storePath))
        throw Error(format("path `%1%' is not valid") % storePath);

	Derivation drv = derivationFromPathTxn(txn, deriver);
	string identifier = drv.stateOutputs.find("state")->second.stateIdentifier;
	string user = drv.stateOutputs.find("state")->second.username;
	
	PathSet currentDerivers = queryDerivers(txn, storePath, identifier, user); 
	PathSet updatedDerivers = mergeNewDerivationIntoListTxn(txn, storePath, deriver, currentDerivers, true);

	Strings data;    	
	for (PathSet::iterator i = updatedDerivers.begin(); i != updatedDerivers.end(); ++i)		//Convert Paths to Strings
       	data.push_back(*i);
	
	nixDB.setStrings(txn, dbDerivers, storePath, data);											//update the derivers db.
	
	nixDB.setString(txn, dbStateInfo, storePath, "");											//update the dbinfo db.	(maybe TODO)
}


/*
 * Returns true or false wheter a store-component has a state component (e.g. has a state dir) or not. 
 * Do NOT confuse this function with isValidStatePath
 */
bool isStateComponentTxn(const Transaction & txn, const Path & storePath)
{
	isValidPathTxn(txn, storePath);
	
	string data;
	bool nonempty = nixDB.queryString(txn, dbStateInfo, storePath, data);
	 
	return nonempty;
}

bool LocalStore::isStateComponent(const Path & storePath)
{
    return nix::isStateComponentTxn(noTxn, storePath);
}


bool isStateDrvPathTxn(const Transaction & txn, const Path & drvPath)
{
	Derivation drv = derivationFromPathTxn(txn, drvPath);
    return isStateDrv(drv);
}

bool isStateDrv(const Derivation & drv)
{
    return (drv.stateOutputs.size() != 0);
}

static Path queryDeriver(const Transaction & txn, const Path & storePath)  
{
    if (!isRealisablePath(txn, storePath))
        throw Error(format("path `%1%' is not valid") % storePath);
    Path deriver;
    
    bool b = nixDB.queryString(txn, dbDerivers, storePath, deriver);
    
    Derivation drv = derivationFromPathTxn(txn, deriver);		
    if (isStateDrv(drv))
    	throw Error(format("This deriver `%1%' is a state deriver, u should use queryDerivers instead of queryDeriver") % deriver);
    	    
    if (b)
        return deriver;
    else
        return "";
}

Path LocalStore::queryDeriver(const Path & path)
{
	return nix::queryDeriver(noTxn, path);
}

//A '*' as argument stands for all identifiers or all users
PathSet queryDerivers(const Transaction & txn, const Path & storePath, const string & identifier, const string & user)
{
	if (!isRealisablePath(txn, storePath))
        throw Error(format("path `%1%' is not valid") % storePath);
	
	if(user == "")
		throw Error(format("The user argument is empty, use queryDeriver(...) for non-state components"));  
	
	Strings alldata;    	
    nixDB.queryStrings(txn, dbDerivers, storePath, alldata);				//get all current derivers

	PathSet filtereddata;
	for (Strings::iterator i = alldata.begin(); i != alldata.end(); ++i) {	//filter on username and identifier
		
		string derivationpath = (*i);
		Derivation drv = derivationFromPathTxn(txn, derivationpath);
		
		if (drv.outputs.size() != 1)
			throw Error(format("The call queryDerivers with storepath %1% is not a statePath") % storePath);
		
		string getIdentifier = drv.stateOutputs.find("state")->second.stateIdentifier;
		string getUser = drv.stateOutputs.find("state")->second.username;

		//printMsg(lvlError, format("queryDerivers '%1%' '%2%' '%3%' '%4%' '%5%'") % derivationpath % getIdentifier % identifier % getUser % user);
		if( (getIdentifier == identifier || identifier == "*") && (getUser == user || user == "*") )
			filtereddata.insert(derivationpath);
	}
	
	return filtereddata;
}

PathSet LocalStore::queryDerivers(const Path & storePath, const string & identifier, const string & user)
{
	return nix::queryDerivers(noTxn, storePath, identifier, user);
}

//Wrapper around converting the drvPath to the statePath
/*
PathSet queryDeriversStatePath(const Transaction & txn, const Path & storePath, const string & identifier, const string & user)
{
	PathSet drvs = queryDerivers(txn, storePath, identifier, user);
	PathSet statePaths;
	for (PathSet::const_iterator i = drvs.begin(); i != drvs.end(); i++){
		Derivation drv = derivationFromPath((*i));
		statePaths.insert(drv.stateOutputs.find("state")->second.statepath);
	}
	return statePaths;
}
*/


const int substituteVersion = 2;


static Substitutes readSubstitutes(const Transaction & txn,
    const Path & srcPath)
{
    Strings ss;
    nixDB.queryStrings(txn, dbSubstitutes, srcPath, ss);

    Substitutes subs;
    
    for (Strings::iterator i = ss.begin(); i != ss.end(); ++i) {
        if (i->size() < 4 || (*i)[3] != 0) {
            /* Old-style substitute.  !!! remove this code
               eventually? */
            break;
        }
        Strings ss2 = unpackStrings(*i);
        if (ss2.size() == 0) continue;
        int version;
        if (!string2Int(ss2.front(), version)) continue;
        if (version != substituteVersion) continue;
        if (ss2.size() != 4) throw Error("malformed substitute");
        Strings::iterator j = ss2.begin();
        j++;
        Substitute sub;
        sub.deriver = *j++;
        sub.program = *j++;
        sub.args = unpackStrings(*j++);
        subs.push_back(sub);
    }

    return subs;
}


static void writeSubstitutes(const Transaction & txn,
    const Path & srcPath, const Substitutes & subs)
{
    Strings ss;

    for (Substitutes::const_iterator i = subs.begin();
         i != subs.end(); ++i)
    {
        Strings ss2;
        ss2.push_back((format("%1%") % substituteVersion).str());
        ss2.push_back(i->deriver);
        ss2.push_back(i->program);
        ss2.push_back(packStrings(i->args));
        ss.push_back(packStrings(ss2));
    }

    nixDB.setStrings(txn, dbSubstitutes, srcPath, ss);
}


void registerSubstitute(const Transaction & txn,
    const Path & srcPath, const Substitute & sub)
{
    assertStorePath(srcPath);
    
    Substitutes subs = readSubstitutes(txn, srcPath);

    if (find(subs.begin(), subs.end(), sub) != subs.end())
        return;

    /* New substitutes take precedence over old ones.  If the
       substitute is already present, it's moved to the front. */
    remove(subs.begin(), subs.end(), sub);
    subs.push_front(sub);
        
    writeSubstitutes(txn, srcPath, subs);
}


Substitutes querySubstitutes(const Transaction & txn, const Path & path)
{
    return readSubstitutes(txn, path);
}


Substitutes LocalStore::querySubstitutes(const Path & path)
{
    return nix::querySubstitutes(noTxn, path);
}


static void invalidatePath(Transaction & txn, const Path & path);


void clearSubstitutes()
{
    Transaction txn(nixDB);
    
    /* Iterate over all paths for which there are substitutes. */
    Paths subKeys;
    nixDB.enumTable(txn, dbSubstitutes, subKeys);
    for (Paths::iterator i = subKeys.begin(); i != subKeys.end(); ++i) {
        
        /* Delete all substitutes for path *i. */
        nixDB.delPair(txn, dbSubstitutes, *i);
        
        /* Maintain the cleanup invariant. */
        if (!isValidPathTxn(txn, *i))
            invalidatePath(txn, *i);
    }

    /* !!! there should be no referrers to any of the invalid
       substitutable paths.  This should be the case by construction
       (the only referrers can be other invalid substitutable paths,
       which have all been removed now). */
    
    txn.commit();
}


static void setHash(const Transaction & txn, const Path & storePath, const Hash & hash, bool stateHash = false)
{
   	nixDB.setString(txn, dbValidPaths, storePath, "sha256:" + printHash(hash));
   	assert(hash.type == htSHA256);
}

static void setStateValid(const Transaction & txn, const Path & statePath, const Path & drvPath)
{
	//printMsg(lvlError, format("Setting statetpath as a valid path in dbValidStatePaths '%1%' with drv '%2%'") % statePath % drvPath);
	nixDB.setString(txn, dbValidStatePaths, statePath, drvPath);
}

static Hash queryHash(const Transaction & txn, const Path & storePath)
{
    string s;
    nixDB.queryString(txn, dbValidPaths, storePath, s);
    string::size_type colon = s.find(':');
    if (colon == string::npos)
        throw Error(format("corrupt hash `%1%' in valid-path entry for `%2%'")
            % s % storePath);
    HashType ht = parseHashType(string(s, 0, colon));
    if (ht == htUnknown)
        throw Error(format("unknown hash type `%1%' in valid-path entry for `%2%'")
            % string(s, 0, colon) % storePath);
    return parseHash(ht, string(s, colon + 1));
}


Hash LocalStore::queryPathHash(const Path & path)
{
    if (!isValidPath(path))
        throw Error(format("path `%1%' is not valid") % path);
    return queryHash(noTxn, path);
}

Path queryStatePathDrvTxn(const Transaction & txn, const Path & statePath)
{
	string s;
    nixDB.queryString(txn, dbValidStatePaths, statePath, s);
    return s;
}

Path LocalStore::queryStatePathDrv(const Path & statePath)
{
    if (!isValidStatePath(statePath))
        throw Error(format("statepath `%1%' is not valid") % statePath);
    return nix::queryStatePathDrvTxn(noTxn, statePath);
}


void registerValidPath(const Transaction & txn,
    const Path & component_or_state_path, const Hash & hash, 
    const PathSet & references, const PathSet & stateReferences,
    const Path & deriver, const unsigned int revision)
{
    ValidPathInfo info;
    info.path = component_or_state_path;
    info.hash = hash;
    info.references = references;									
    info.stateReferences = stateReferences;   
    info.revision = revision;
    info.deriver = deriver;
    ValidPathInfos infos;
    infos.push_back(info);
    registerValidPaths(txn, infos);
}


void registerValidPaths(const Transaction & txn, const ValidPathInfos & infos)
{
    PathSet newPaths;
    for (ValidPathInfos::const_iterator i = infos.begin(); i != infos.end(); ++i)
        newPaths.insert(i->path);
		
    for (ValidPathInfos::const_iterator i = infos.begin(); i != infos.end(); ++i)
    {
        //Check the type of path: component or state
        bool isStorePath_b;
        if(isStorePath(i->path)){
        	assertStorePath(i->path);
        	isStorePath_b = true;
        }
        else{
        	assertStatePath(i->path);
        	isStorePath_b = false;
        }

        debug(format("registering path `%1%'") % i->path);
        
        if(isStorePath_b)
        	setHash(txn, i->path, i->hash);								//set compont path valid
		else
			setStateValid(txn, i->path, i->deriver);					//or set state path valid 

        setReferences(txn, i->path, i->references, i->stateReferences, i->revision);
        
        /* Check that all referenced paths are also valid (or about to) become valid). */
        for (PathSet::iterator j = i->references.begin(); j != i->references.end(); ++j)
            if (!isValidPathTxn(txn, *j) && newPaths.find(*j) == newPaths.end())
                throw Error(format("cannot register path `%1%' as valid, since its reference `%2%' is invalid") % i->path % *j);
		
		//We cannot check the statePath since registerValidPath is called twice, first for the component path, and then for the state path....
				
		if(isStorePath_b){		
        	setDeriver(txn, i->path, i->deriver);
		}
        
        //TODO maybe also set a state deriver into dbStateDerivers .... well state is already linked to a drvpath in dbValidStatePaths ....
    }
}


/* Invalidate a path. The caller is responsible for checking that
   there are no referrers. */
static void invalidatePath(Transaction & txn, const Path & path)			//TODO Adjust for state paths????
{
    debug(format("unregistering path `%1%'") % path);

    /* Clear the `references' entry for this path, as well as the
       inverse `referrers' entries, and the `derivers' entry; but only
       if there are no substitutes for this path.  This maintains the
       cleanup invariant. */
    if (querySubstitutes(txn, path).size() == 0) {
        setReferences(txn, path, PathSet(), PathSet(), -1);					//Set last references empty	(TODO is this ok?)
        nixDB.delPair(txn, dbDerivers, path);								//TODO!!!!! also for state Derivers!!!!!!!!!!!!!!!
    }
    
    nixDB.delPair(txn, dbValidPaths, path);									
}


Path LocalStore::addToStore(const Path & _srcPath, bool fixed,
    bool recursive, string hashAlgo, PathFilter & filter)
{
    Path srcPath(absPath(_srcPath));
    debug(format("adding `%1%' to the store") % srcPath);

    std::pair<Path, Hash> pr =
        computeStorePathForPath(srcPath, fixed, recursive, hashAlgo, filter);
    Path & dstPath(pr.first);
    Hash & h(pr.second);

    addTempRoot(dstPath);

    if (!isValidPath(dstPath)) {

        /* The first check above is an optimisation to prevent
           unnecessary lock acquisition. */

        PathLocks outputLock(singleton<PathSet, Path>(dstPath));

        if (!isValidPath(dstPath)) {

            if (pathExists(dstPath)) deletePathWrapped(dstPath);

            copyPath(srcPath, dstPath, filter);

            Hash h2 = hashPath(htSHA256, dstPath, filter);
            if (h != h2)
                throw Error(format("contents of `%1%' changed while copying it to `%2%' (%3% -> %4%)")
                    % srcPath % dstPath % printHash(h) % printHash(h2));

            canonicalisePathMetaData(dstPath);
            
            Transaction txn(nixDB);
            registerValidPath(txn, dstPath, h, PathSet(), PathSet(), "", -1);			//TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!! CHECK (probabyly ok?)
            txn.commit();
        }
        

        outputLock.setDeletion(true);
    }
    
    return dstPath;
}


//Gets all derivations ...
Path LocalStore::addTextToStore(const string & suffix, const string & s,
    const PathSet & references)
{
    Path dstPath = computeStorePathForText(suffix, s, references);
    
    //printMsg(lvlError, format("addTextToStore: %1%") % dstPath);
    
    addTempRoot(dstPath);

    if (!isValidPath(dstPath)) {

        PathLocks outputLock(singleton<PathSet, Path>(dstPath));

        if (!isValidPath(dstPath)) {

            if (pathExists(dstPath)) deletePathWrapped(dstPath);

            writeStringToFile(dstPath, s);

            canonicalisePathMetaData(dstPath);
            
            Transaction txn(nixDB);
            registerValidPath(txn, dstPath, hashPath(htSHA256, dstPath), references, PathSet(), "", 0);	//There are no stateReferences in drvs..... so we dont need to register them (I think)
            																							//A drvs has also no statepath, so that is ok...
            txn.commit();
        }

        outputLock.setDeletion(true);
    }

    return dstPath;
}


struct HashAndWriteSink : Sink
{
    Sink & writeSink;
    HashSink hashSink;
    bool hashing;
    HashAndWriteSink(Sink & writeSink) : writeSink(writeSink), hashSink(htSHA256)
    {
        hashing = true;
    }
    virtual void operator ()
        (const unsigned char * data, unsigned int len)
    {
        writeSink(data, len);
        if (hashing) hashSink(data, len);
    }
};


#define EXPORT_MAGIC 0x4558494e


static void checkSecrecy(const Path & path)
{
    struct stat st;
    if (stat(path.c_str(), &st))
        throw SysError(format("getting status of `%1%'") % path);
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        throw Error(format("file `%1%' should be secret (inaccessible to everybody else)!") % path);
}


void LocalStore::exportPath(const Path & path, bool sign,
    Sink & sink)
{
    assertStorePath(path);

    /* Wrap all of this in a transaction to make sure that we export
       consistent metadata. */
    Transaction txn(nixDB);
    addTempRoot(path);
    if (!isValidPath(path))
        throw Error(format("path `%1%' is not valid") % path);

    HashAndWriteSink hashAndWriteSink(sink);
    
    dumpPath(path, hashAndWriteSink);

    writeInt(EXPORT_MAGIC, hashAndWriteSink);

    writeString(path, hashAndWriteSink);
    
    PathSet references;
    nix::queryXReferencesTxn(txn, path, references, true, 0);		//TODO we can only now export the final revision	//TODO also export the state references ???
    writeStringSet(references, hashAndWriteSink);

    Path deriver = nix::queryDeriver(txn, path);
    writeString(deriver, hashAndWriteSink);

    if (sign) {
        Hash hash = hashAndWriteSink.hashSink.finish();
        hashAndWriteSink.hashing = false;

        writeInt(1, hashAndWriteSink);
        
        Path tmpDir = createTempDir();
        AutoDelete delTmp(tmpDir);
        Path hashFile = tmpDir + "/hash";
        writeStringToFile(hashFile, printHash(hash));

        Path secretKey = nixConfDir + "/signing-key.sec";
        checkSecrecy(secretKey);

        Strings args;
        args.push_back("rsautl");
        args.push_back("-sign");
        args.push_back("-inkey");
        args.push_back(secretKey);
        args.push_back("-in");
        args.push_back(hashFile);
        string signature = runProgram(OPENSSL_PATH, true, args);

        writeString(signature, hashAndWriteSink);
        
    } else
        writeInt(0, hashAndWriteSink);

    txn.commit();
}


struct HashAndReadSource : Source
{
    Source & readSource;
    HashSink hashSink;
    bool hashing;
    HashAndReadSource(Source & readSource) : readSource(readSource), hashSink(htSHA256)
    {
        hashing = true;
    }
    virtual void operator ()
        (unsigned char * data, unsigned int len)
    {
        readSource(data, len);
        if (hashing) hashSink(data, len);
    }
};


Path LocalStore::importPath(bool requireSignature, Source & source)
{
    HashAndReadSource hashAndReadSource(source);
    
    /* We don't yet know what store path this archive contains (the
       store path follows the archive data proper), and besides, we
       don't know yet whether the signature is valid. */
    Path tmpDir = createTempDir(nixStore);
    AutoDelete delTmp(tmpDir);
    Path unpacked = tmpDir + "/unpacked";

    restorePath(unpacked, hashAndReadSource);

    unsigned int magic = readInt(hashAndReadSource);
    if (magic != EXPORT_MAGIC)
        throw Error("Nix archive cannot be imported; wrong format");

    Path dstPath = readStorePath(hashAndReadSource);

    PathSet references = readStorePaths(hashAndReadSource);
    
    //TODO TODO also ..??!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    PathSet stateReferences;

    Path deriver = readString(hashAndReadSource);
    if (deriver != "") assertStorePath(deriver);

    Hash hash = hashAndReadSource.hashSink.finish();
    hashAndReadSource.hashing = false;

    bool haveSignature = readInt(hashAndReadSource) == 1;

    if (requireSignature && !haveSignature)
        throw Error("imported archive lacks a signature");
    
    if (haveSignature) {
        string signature = readString(hashAndReadSource);

        if (requireSignature) {
            Path sigFile = tmpDir + "/sig";
            writeStringToFile(sigFile, signature);

            Strings args;
            args.push_back("rsautl");
            args.push_back("-verify");
            args.push_back("-inkey");
            args.push_back(nixConfDir + "/signing-key.pub");
            args.push_back("-pubin");
            args.push_back("-in");
            args.push_back(sigFile);
            string hash2 = runProgram(OPENSSL_PATH, true, args);

            /* Note: runProgram() throws an exception if the signature
               is invalid. */

            if (printHash(hash) != hash2)
                throw Error(
                    "signed hash doesn't match actual contents of imported "
                    "archive; archive could be corrupt, or someone is trying "
                    "to import a Trojan horse");
        }
    }

    /* Do the actual import. */

    /* !!! way too much code duplication with addTextToStore() etc. */
    addTempRoot(dstPath);

    if (!isValidPath(dstPath)) {

        PathLocks outputLock(singleton<PathSet, Path>(dstPath));

        if (!isValidPath(dstPath)) {

            if (pathExists(dstPath)) deletePathWrapped(dstPath);

            if (rename(unpacked.c_str(), dstPath.c_str()) == -1)
                throw SysError(format("cannot move `%1%' to `%2%'")
                    % unpacked % dstPath);

            canonicalisePathMetaData(dstPath);
            
            Transaction txn(nixDB);
            /* !!! if we were clever, we could prevent the hashPath()
               here. */
            if (!isValidPath(deriver)) deriver = "";
            registerValidPath(txn, dstPath, 												//TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!! replace how about state paths ??????
                hashPath(htSHA256, dstPath), references, stateReferences, deriver, 0);
            txn.commit();
        }
        
        outputLock.setDeletion(true);
    }
    
    return dstPath;
}


void deleteFromStore(const Path & _path, unsigned long long & bytesFreed)
{
    bytesFreed = 0;
    Path path(canonPath(_path));

    assertStorePath(path);

    Transaction txn(nixDB);
    if (isValidPathTxn(txn, path)) {
        PathSet referrers = getReferrersTxn(txn, path, -1);		//delete latest referrers (TODO?)
        for (PathSet::iterator i = referrers.begin();
             i != referrers.end(); ++i)
            if (*i != path && isValidPathTxn(txn, *i))
                throw PathInUse(format("cannot delete path `%1%' because it is in use by path `%2%'") % path % *i);
        invalidatePath(txn, path);
        
        //TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! Also delete/invalidate stateReferrers?????
    }
    txn.commit();

    deletePathWrapped(path, bytesFreed);
}


void verifyStore(bool checkContents)
{
    Transaction txn(nixDB);

    
    printMsg(lvlInfo, "checking path existence");

    Paths paths;
    PathSet validPaths;
    nixDB.enumTable(txn, dbValidPaths, paths);

    for (Paths::iterator i = paths.begin(); i != paths.end(); ++i) {
        if (!pathExists(*i)) {
            printMsg(lvlError, format("path `%1%' disappeared") % *i);
            invalidatePath(txn, *i);
        } else if (!isStorePath(*i)) {
            printMsg(lvlError, format("path `%1%' is not in the Nix store") % *i);
            invalidatePath(txn, *i);
        } else {
            if (checkContents) {
                debug(format("checking contents of `%1%'") % *i);
                Hash expected = queryHash(txn, *i);
                Hash current = hashPath(expected.type, *i);
                if (current != expected) {
                    printMsg(lvlError, format("path `%1%' was modified! "
                                 "expected hash `%2%', got `%3%'")
                        % *i % printHash(expected) % printHash(current));
                }
            }
            validPaths.insert(*i);
        }
    }


    printMsg(lvlInfo, "checking path realisability");
    
    /* "Realisable" paths are those that are valid or have a
       substitute. */
    PathSet realisablePaths(validPaths);
    
    
    //TODO Do also for validStatePaths

    /* Check that the values of the substitute mappings are valid
       paths. */ 
    Paths subKeys;
    nixDB.enumTable(txn, dbSubstitutes, subKeys);
    for (Paths::iterator i = subKeys.begin(); i != subKeys.end(); ++i) {
        Substitutes subs = readSubstitutes(txn, *i);
        if (!isStorePath(*i)) {
            printMsg(lvlError, format("removing substitutes for non-store path `%1%'") % *i);
            nixDB.delPair(txn, dbSubstitutes, *i);
        }
        else if (subs.size() == 0)
            nixDB.delPair(txn, dbSubstitutes, *i);
        else
	    realisablePaths.insert(*i);
    }
    

    /* Check the cleanup invariant: only realisable paths can have
       `references', `referrers', or `derivers' entries. */


    /* Check the `derivers' table. */
    printMsg(lvlInfo, "checking the derivers table");
    Paths deriversKeys;
    nixDB.enumTable(txn, dbDerivers, deriversKeys);
    for (Paths::iterator i = deriversKeys.begin();
         i != deriversKeys.end(); ++i)
    {
        if (realisablePaths.find(*i) == realisablePaths.end()) {
            printMsg(lvlError, format("removing deriver entry for unrealisable path `%1%'")
                % *i);
            nixDB.delPair(txn, dbDerivers, *i);
        }
        else {
            Path deriver = queryDeriver(txn, *i);
            if (!isStorePath(deriver)) {
                printMsg(lvlError, format("removing corrupt deriver `%1%' for `%2%'")
                    % deriver % *i);
                nixDB.delPair(txn, dbDerivers, *i);
            }
        }
    }

    /* Check the `references' table. */
    //TODO TODO Do the exact same thing for the other dbreferres and references
    printMsg(lvlInfo, "checking the references table");
    Paths referencesKeys;
    nixDB.enumTable(txn, dbComponentComponentReferences, referencesKeys);
    for (Paths::iterator i = referencesKeys.begin();
         i != referencesKeys.end(); ++i)
    {
        if (realisablePaths.find(*i) == realisablePaths.end()) {
            printMsg(lvlError, format("removing references entry for unrealisable path `%1%'")
                % *i);
            setReferences(txn, *i, PathSet(), PathSet(), 0);		//TODO?
        }
        else {
            bool isValid = validPaths.find(*i) != validPaths.end();
            PathSet references;
            queryXReferencesTxn(txn, *i, references, true, -1);				//TODO
            for (PathSet::iterator j = references.begin();
                 j != references.end(); ++j)
            {
                
                /*
                string dummy;
                if (!nixDB.queryString(txn, dbComponentComponentReferrers, addPrefix(*j, *i), dummy)) {
                    printMsg(lvlError, format("adding missing referrer mapping from `%1%' to `%2%'")
                        % *j % *i);
                    nixDB.setString(txn, dbComponentComponentReferrers, addPrefix(*j, *i), "");
                }
                */
                
                if (isValid && validPaths.find(*j) == validPaths.end()) {
                    printMsg(lvlError, format("incomplete closure: `%1%' needs missing `%2%'")
                        % *i % *j);
                }
            }
        }
    }
    

    //TODO Check stateinfo and statecounters table
	
    
    txn.commit();
}

void setStatePathsIntervalTxn(const Transaction & txn, const PathSet & statePaths, const IntVector & intervals, bool allZero)
{
	if(!allZero && statePaths.size() != intervals.size()){
		throw Error("the number of statepaths and intervals must be equal");
	} 

	int n=0;
    for (PathSet::iterator i = statePaths.begin(); i != statePaths.end(); ++i)
    {
        //printMsg(lvlError, format("Set interval of PATH: %1%") % *i);
        
        int interval=0;
        if(!allZero)
        	interval = intervals.at(n);
                
        nixDB.setString(txn, dbStateCounters, *i, int2String(interval));
        n++;
    }
}

void LocalStore::setStatePathsInterval(const PathSet & statePaths, const IntVector & intervals, bool allZero)
{
	Transaction txn(nixDB);
    nix::setStatePathsIntervalTxn(txn, statePaths, intervals, allZero);
    txn.commit();
}

IntVector getStatePathsIntervalTxn(const Transaction & txn, const PathSet & statePaths)
{
	string data;
	Paths referers;
	
	IntVector intervals;
    for (PathSet::iterator i = statePaths.begin(); i != statePaths.end(); ++i)
    {
    	nixDB.queryString(txn, dbStateCounters, *i, data);

		//Check if every key returns a value from the db
    	int n;
    	if (!string2Int(data, n)) 
    		throw Error(format("Statepath `%1%' has returned no valid interval from the database") % *i);
    	intervals.push_back(n);
    }        

    return intervals;
}

IntVector LocalStore::getStatePathsInterval(const PathSet & statePaths)
{
    return nix::getStatePathsIntervalTxn(noTxn, statePaths);
}


/* Place in `paths' the set of paths that are required to `realise'
   the given store path, i.e., all paths necessary for valid
   deployment of the path.  For a derivation, this is the union of
   requisites of the inputs, plus the derivation; for other store
   paths, it is the set of paths in the FS closure of the path.  If
   `includeOutputs' is true, include the requisites of the output
   paths of derivations as well.

   Note that this function can be used to implement three different
   deployment policies:

   - Source deployment (when called on a derivation).
   - Binary deployment (when called on an output path).
   - Source/binary deployment (when called on a derivation with
     `includeOutputs' set to true).
     
     
     TODO Change comment, this can also take state paths
*/
void storePathRequisites(const Path & storeOrstatePath, const bool includeOutputs, PathSet & paths, const bool withComponents, const bool withState, const unsigned int revision)
{
	nix::storePathRequisitesTxn(noTxn, storeOrstatePath, includeOutputs, paths, withComponents, withState, revision);
}

void storePathRequisitesTxn(const Transaction & txn, const Path & storeOrstatePath, const bool includeOutputs, PathSet & paths, const bool withComponents, const bool withState, const unsigned int revision)
{
    computeFSClosureTxn(txn, storeOrstatePath, paths, withComponents, withState, revision);

    if (includeOutputs) {
        for (PathSet::iterator i = paths.begin();
             i != paths.end(); ++i)
            if (isDerivation(*i)) {
                Derivation drv = derivationFromPathTxn(txn, *i);
                for (DerivationOutputs::iterator j = drv.outputs.begin();
                     j != drv.outputs.end(); ++j)
                    if (isValidPathTxn(txn, j->second.path))
                        computeFSClosureTxn(txn, j->second.path, paths, withComponents, withState, revision);
            }
    }
}

void LocalStore::storePathRequisites(const Path & storeOrstatePath, const bool includeOutputs, PathSet & paths, const bool withComponents, const bool withState, const unsigned int revision)
{
    nix::storePathRequisites(storeOrstatePath, includeOutputs, paths, withComponents, withState, revision);
}

void queryAllValidPathsTxn(const Transaction & txn, PathSet & allComponentPaths, PathSet & allStatePaths)
{
	Paths allComponentPaths2;
    Paths allStatePaths2;
    nixDB.enumTable(txn, dbValidPaths, allComponentPaths2);
    nixDB.enumTable(txn, dbValidStatePaths, allStatePaths2);
    allComponentPaths.insert(allComponentPaths2.begin(), allComponentPaths2.end());
    allStatePaths.insert(allStatePaths2.begin(), allStatePaths2.end());
    
    for (PathSet::iterator i = allComponentPaths.begin(); i != allComponentPaths.end(); ++i)
    		debug(format("allComponentPaths: %1%") % *i);
    for (PathSet::iterator i = allStatePaths.begin(); i != allStatePaths.end(); ++i)
    		debug(format("allStatePaths: %1%") % *i);
    		
}


void setStateRevisionsTxn(const Transaction & txn, const RevisionClosure & revisions, const Path & rootStatePath, const string & comment)
{
	nixDB.setStateRevisions(txn, dbStateRevisions, dbStateRevisionsComments, dbStateSnapshots, revisions, rootStatePath, comment);	
}

void LocalStore::setStateRevisions(const RevisionClosure & revisions, const Path & rootStatePath, const string & comment)
{
	nix::setStateRevisionsTxn(noTxn, revisions, rootStatePath, comment);	
}

bool queryStateRevisionsTxn(const Transaction & txn, const Path & statePath, RevisionClosure & revisions, RevisionClosureTS & timestamps, const unsigned int revision)
{
	return nixDB.queryStateRevisions(txn, dbStateRevisions, dbStateSnapshots, statePath, revisions, timestamps, revision);
}

bool LocalStore::queryStateRevisions(const Path & statePath, RevisionClosure & revisions, RevisionClosureTS & timestamps, const unsigned int revision)
{
	return nix::queryStateRevisionsTxn(noTxn, statePath, revisions, timestamps, revision);
}

bool queryAvailableStateRevisionsTxn(const Transaction & txn, const Path & statePath, RevisionInfos & revisions)
{
	 return nixDB.queryAvailableStateRevisions(txn, dbStateRevisions, dbStateRevisionsComments, statePath, revisions);
}

bool LocalStore::queryAvailableStateRevisions(const Path & statePath, RevisionInfos & revisions)
{
	return nix::queryAvailableStateRevisionsTxn(noTxn, statePath, revisions);
}

Snapshots LocalStore::commitStatePath(const Path & statePath)
{
	Transaction txn(nixDB);
	Snapshots ss = nix::commitStatePathTxn(txn, statePath);
	txn.commit();
	return ss;	
}

void LocalStore::scanAndUpdateAllReferences(const Path & statePath, const bool recursive)
{
	Transaction txn(nixDB);
	if(recursive)
		nix::scanAndUpdateAllReferencesRecusivelyTxn(txn, statePath);
	else{
		PathSet empty;
		nix::scanAndUpdateAllReferencesTxn(txn, statePath, empty, empty);
	}
	txn.commit();
}

void setSolidStateReferencesTxn(const Transaction & txn, const Path & statePath, const PathSet & paths)
{
	Strings ss = Strings(paths.begin(), paths.end());
	nixDB.setStrings(txn, dbSolidStateReferences, statePath, ss);
}

bool querySolidStateReferencesTxn(const Transaction & txn, const Path & statePath, PathSet & paths)
{
	Strings ss;
	bool notempty = nixDB.queryStrings(txn, dbSolidStateReferences, statePath, ss);
	paths.insert(ss.begin(), ss.end());
	return notempty;
}

void setSharedStateTxn(const Transaction & txn, const Path & fromExisting, const Path & toNew)
{
	//TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! LEGALITY CHECK IF THE PATH MAY BE SHARED !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	//TODO

	//Remove earlier entries
	nixDB.delPair(txn, dbSharedState, toNew);

	//Set new entry
	nixDB.setString(txn, dbSharedState, toNew, fromExisting);
}

void LocalStore::setSharedState(const Path & fromExisting, const Path & toNew)
{
	Transaction txn(nixDB);
	setSharedStateTxn(txn, fromExisting, toNew);
	txn.commit();
}


bool querySharedStateTxn(const Transaction & txn, const Path & statePath, Path & shared_with)
{
	return nixDB.queryString(txn, dbSharedState, statePath, shared_with);
}

Path toNonSharedPathTxn(const Transaction & txn, const Path & statePath)
{
	//we do querySharedStateTxn until there the current path is not a shared path anymore  
	Path sharedPath;
	Path returnedPath = statePath;
	
	while(querySharedStateTxn(txn, returnedPath, sharedPath))
		returnedPath = sharedPath;
	
	return returnedPath;
}

PathSet toNonSharedPathSetTxn(const Transaction & txn, const PathSet & statePaths)
{
	PathSet real_statePaths;

	//we loop over all paths in the list
	for (PathSet::const_iterator i = statePaths.begin(); i != statePaths.end(); ++i)
		real_statePaths.insert(toNonSharedPathTxn(txn, *i));
		
	return real_statePaths;
}

PathSet LocalStore::toNonSharedPathSet(const PathSet & statePaths)
{
	return toNonSharedPathSetTxn(noTxn, statePaths);
}

void setStateComponentReferencesTxn(const Transaction & txn, const Path & statePath, const Strings & references, const unsigned int revision, const unsigned int timestamp)
{
	nixDB.setStateReferences(txn, dbStateComponentReferences, dbStateRevisions, statePath, references, revision, timestamp);
}

void setStateStateReferencesTxn(const Transaction & txn, const Path & statePath, const Strings & references, const unsigned int revision, const unsigned int timestamp)
{
	nixDB.setStateReferences(txn, dbStateStateReferences, dbStateRevisions, statePath, references, revision, timestamp);
}

//Lookups which statePaths directy share (point to) statePath
PathSet getDirectlySharedWithPathSetTxn(const Transaction & txn, const Path & statePath)
{
	PathSet statePaths;

	//enumtable
	Strings keys;
	nixDB.enumTable(txn, dbSharedState, keys);
	
	for (Strings::iterator i = keys.begin(); i != keys.end(); ++i){
		Path shared_with;
		nixDB.queryString(txn, dbSharedState, *i, shared_with);
		if (shared_with == statePath)
			statePaths.insert(*i);
	}

	return statePaths;
}

PathSet getSharedWithPathSetRecTxn_private(const Transaction & txn, const Path & statePath, PathSet & statePaths)
{
	//Get all paths pointing to statePath
	PathSet newStatePaths = getDirectlySharedWithPathSetTxn(txn, statePath);
		
	//go into recursion to see if there are more paths indirectly pointing to statePath
	for (PathSet::iterator i = newStatePaths.begin(); i != newStatePaths.end(); ++i){
		
		//Only recurse on the really new statePaths to prevent infinite recursion
		if(statePaths.find(*i) == statePaths.end())	{ 
			statePaths.insert(*i);
			statePaths = pathSets_union(statePaths, getSharedWithPathSetRecTxn_private(txn, *i, statePaths));
		}
	}
	
	return statePaths;
}

PathSet getSharedWithPathSetRecTxn(const Transaction & txn, const Path & statePath)
{
	//To no shared state path
	Path statePath_ns = toNonSharedPathTxn(txn, statePath);
	
	//Also insert non-shared state path if it doenst equal statePath 
	PathSet statePaths;
	if(statePath_ns != statePath)
		statePaths.insert(statePath_ns);
	
	//Make the call to get all shared with paths
	PathSet result = getSharedWithPathSetRecTxn_private(txn, statePath_ns, statePaths);
	
	//Remove yourself from the list eventually
	result.erase(statePath);
	
	return result;
}


void LocalStore::revertToRevision(const Path & statePath, const unsigned int revision_arg, const bool recursive)
{
	Transaction txn(nixDB);
	revertToRevisionTxn(txn, statePath, revision_arg, recursive);
	txn.commit();	
}


/* Upgrade from schema 1 (Nix <= 0.7) to schema 2 (Nix >= 0.8). */
static void upgradeStore07()
{
    printMsg(lvlError, "upgrading Nix store to new schema (this may take a while)...");

    Transaction txn(nixDB);

    Paths validPaths2;
    nixDB.enumTable(txn, dbValidPaths, validPaths2);
    PathSet validPaths(validPaths2.begin(), validPaths2.end());

    std::cerr << "hashing paths...";
    int n = 0;
    for (PathSet::iterator i = validPaths.begin(); i != validPaths.end(); ++i) {
        checkInterrupt();
        string s;
        nixDB.queryString(txn, dbValidPaths, *i, s);
        if (s == "") {
            Hash hash = hashPath(htSHA256, *i);
            setHash(txn, *i, hash);
            std::cerr << ".";
            if (++n % 1000 == 0) {
                txn.commit();
                txn.begin(nixDB);
            }
        }
    }
    std::cerr << std::endl;

    txn.commit();

    txn.begin(nixDB);
    
    std::cerr << "processing closures...";
    for (PathSet::iterator i = validPaths.begin(); i != validPaths.end(); ++i) {
        checkInterrupt();
        if (i->size() > 6 && string(*i, i->size() - 6) == ".store") {
            ATerm t = ATreadFromNamedFile(i->c_str());
            if (!t) throw Error(format("cannot read aterm from `%1%'") % *i);

            ATermList roots, elems;
            if (!matchOldClosure(t, roots, elems)) continue;

            for (ATermIterator j(elems); j; ++j) {

                ATerm path2;
                ATermList references2;
                if (!matchOldClosureElem(*j, path2, references2)) continue;

                Path path = aterm2String(path2);
                if (validPaths.find(path) == validPaths.end())
                    /* Skip this path; it's invalid.  This is a normal
                       condition (Nix <= 0.7 did not enforce closure
                       on closure store expressions). */
                    continue;

                PathSet references;
                for (ATermIterator k(references2); k; ++k) {
                    Path reference = aterm2String(*k);
                    if (validPaths.find(reference) == validPaths.end())
                        /* Bad reference.  Set it anyway and let the
                           user fix it. */
                        printMsg(lvlError, format("closure `%1%' contains reference from `%2%' "
                                     "to invalid path `%3%' (run `nix-store --verify')")
                            % *i % path % reference);
                    references.insert(reference);
                }

                PathSet prevReferences;
                queryXReferencesTxn(txn, path, prevReferences, true, -1);
                if (prevReferences.size() > 0 && references != prevReferences)
                    printMsg(lvlError, format("warning: conflicting references for `%1%'") % path);

                if (references != prevReferences)
                    setReferences(txn, path, references, PathSet(), 0);
            }
            
            std::cerr << ".";
        }
    }
    std::cerr << std::endl;

    /* !!! maybe this transaction is way too big */
    txn.commit();
}


/* Upgrade from schema 2 (0.8 <= Nix <= 0.9) to schema 3 (Nix >=
   0.10).  The only thing to do here is to upgrade the old `referer'
   table (which causes quadratic complexity in some cases) to the new
   (and properly spelled) `referrer' table. */
static void upgradeStore09() 
{
    /* !!! we should disallow concurrent upgrades */
    
    printMsg(lvlError, "upgrading Nix store to new schema (this may take a while)...");

    if (!pathExists(nixDBPath + "/referers")) return;

	/*
    Transaction txn(nixDB);
    
    std::cerr << "converting referers to referrers...";

    TableId dbReferers = nixDB.openTable("referers"); // sic!

    Paths referersKeys;
    nixDB.enumTable(txn, dbReferers, referersKeys);

    int n = 0;
    for (Paths::iterator i = referersKeys.begin();
         i != referersKeys.end(); ++i)
    {
        Paths referers;
        nixDB.queryStrings(txn, dbReferers, *i, referers);
        for (Paths::iterator j = referers.begin();
             j != referers.end(); ++j)
            nixDB.setString(txn, dbComponentComponentReferrers, addPrefix(*i, *j), "");
        if (++n % 1000 == 0) {
            txn.commit();
            txn.begin(nixDB);
            std::cerr << "|";
        }
        std::cerr << ".";
    }

    txn.commit();
    
    
    std::cerr << std::endl;

    nixDB.closeTable(dbReferers);
	*/

    nixDB.deleteTable("referers");
    
}

 
}
