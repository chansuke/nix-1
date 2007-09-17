#include "store-api.hh"
#include "globals.hh"
#include "util.hh"


namespace nix {


bool StoreAPI::hasSubstitutes(const Path & path)
{
    return !querySubstitutes(path).empty();
}


bool isInStore(const Path & path)
{
    return path[0] == '/'
        && string(path, 0, nixStore.size()) == nixStore
        && path.size() >= nixStore.size() + 2
        && path[nixStore.size()] == '/';
}

bool isInStateStore(const Path & path)
{
    return path[0] == '/'
        && string(path, 0, nixStoreState.size()) == nixStoreState
        && path.size() >= nixStoreState.size() + 2
        && path[nixStoreState.size()] == '/';
}

bool isStorePath(const Path & path)
{
    return isInStore(path)
        && path.find('/', nixStore.size() + 1) == Path::npos;
}

bool isStatePath(const Path & path)
{
    return isInStateStore(path)
        && path.find('/', nixStoreState.size() + 1) == Path::npos;
}

void assertStorePath(const Path & path)
{
    if (!isStorePath(path))
        throw Error(format("component path `%1%' is not in the Nix store") % path);
}

void assertStatePath(const Path & path)
{
    if (!isStatePath(path))
        throw Error(format("state path `%1%' is not in the Nix state-store") % path);
}


Path toStorePath(const Path & path)
{
    if (!isInStore(path))
        throw Error(format("path `%1%' is not in the Nix store (2)") % path);
    Path::size_type slash = path.find('/', nixStore.size() + 1);
    if (slash == Path::npos)
        return path;
    else
        return Path(path, 0, slash);
}

Path toStoreOrStatePath(const Path & path)
{
    bool isStorePath = isInStore(path);
    bool isStateStore = isInStateStore(path);
    
    if (!isStorePath && !isStateStore)
        throw Error(format("path `%1%' is not in the Nix store or Nix state store") % path);
    
    Path::size_type slash;
    if(isStorePath)
    	slash = path.find('/', nixStore.size() + 1);
    else
    	slash = path.find('/', nixStoreState.size() + 1);
    
    if (slash == Path::npos)
        return path;
    else
        return Path(path, 0, slash);
}


void checkStoreName(const string & name)
{
    string validChars = "+-._?=";
    /* Disallow names starting with a dot for possible security
       reasons (e.g., "." and ".."). */
    if (string(name, 0, 1) == ".")
        throw Error(format("illegal name: `%1%'") % name);
    for (string::const_iterator i = name.begin(); i != name.end(); ++i)
        if (!((*i >= 'A' && *i <= 'Z') ||
              (*i >= 'a' && *i <= 'z') ||
              (*i >= '0' && *i <= '9') ||
              validChars.find(*i) != string::npos))
        {
            throw Error(format("invalid character `%1%' in name `%2%'")
                % *i % name);
        }
}


Path makeStorePath(const string & type, const Hash & hash, const string & suffix)
{
    /* e.g., "source:sha256:1abc...:/nix/store:foo.tar.gz" */
    string s = type + ":sha256:" + printHash(hash) + ":"
        + nixStore + ":" + suffix;

    checkStoreName(suffix);

    return nixStore + "/"
        + printHash32(compressHash(hashString(htSHA256, s), 20))		//TODO maybe also add a suffix_stateIdentifier when: there is state & no runtime state args & ... ?
        + "-" + suffix;
}

Path makeStatePath(const string & componentHash, const string & suffix, const string & stateIdentifier)
{
    string suffix_stateIdentifier = stateIdentifier;
   	suffix_stateIdentifier = "-" + suffix_stateIdentifier; 
    
    string username = queryCallingUsername();					//Should NOT be fake-able
    
    /* e.g., "source:sha256:1abc...:/nix/store:foo.tar.gz" */
    string s = ":sha256:" + componentHash + ":"
        + nixStoreState + ":" + suffix + ":" + stateIdentifier + ":" + username;

    checkStoreName(suffix);
    checkStoreName(stateIdentifier);

	return nixStoreState + "/"
        	+ printHash32(compressHash(hashString(htSHA256, s), 20))
        	+ "-" + suffix + suffix_stateIdentifier;
}

void checkStatePath(const Derivation & drv)
{
	Path drvPath = drv.stateOutputs.find("state")->second.statepath;

    string componentHash = drv.stateOutputs.find("state")->second.componentHash;
    string suffix = drv.env.find("name")->second;
	string stateIdentifier = drv.stateOutputs.find("state")->second.stateIdentifier;
	
	
	//TODO Name check
	
	
	
    Path calculatedPath = makeStatePath(componentHash, suffix, stateIdentifier);				//TODO INCLUDE USER !!!!!!!!!!!!
	
	//TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! calculatedPath IS NOT CORRECT ANYMORE !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	
	printMsg(lvlError, format("Checking statePath validity: %1% %2%") % drvPath % calculatedPath);		
		
    if(drvPath != calculatedPath)
    	Error(format("The statepath from the Derivation does not match the recalculated statepath, are u trying to spoof the statepath?"));
}

Path makeFixedOutputPath(bool recursive,
    string hashAlgo, Hash hash, string name)
{
    /* !!! copy/paste from primops.cc */
    Hash h = hashString(htSHA256, "fixed:out:"
        + (recursive ? (string) "r:" : "") + hashAlgo + ":"
        + printHash(hash) + ":"
        + "");
    return makeStorePath("output:out", h, name);
}


std::pair<Path, Hash> computeStorePathForPath(const Path & srcPath,
    bool fixed, bool recursive, string hashAlgo, PathFilter & filter)
{
    Hash h = hashPath(htSHA256, srcPath, filter);

    string baseName = baseNameOf(srcPath);

    Path dstPath;
    
    if (fixed) {
        HashType ht(parseHashType(hashAlgo));
        Hash h2 = recursive ? hashPath(ht, srcPath, filter) : hashFile(ht, srcPath);
        dstPath = makeFixedOutputPath(recursive, hashAlgo, h2, baseName);
    }
        
    else dstPath = makeStorePath("source", h, baseName);

    return std::pair<Path, Hash>(dstPath, h);
}


Path computeStorePathForText(const string & suffix, const string & s,
    const PathSet & references)
{
    Hash hash = hashString(htSHA256, s);
    /* Stuff the references (if any) into the type.  This is a bit
       hacky, but we can't put them in `s' since that would be
       ambiguous. */
    string type = "text";
    for (PathSet::const_iterator i = references.begin(); i != references.end(); ++i) {
        type += ":";
        type += *i;
    }

    return makeStorePath(type, hash, suffix);
}


}


#include "local-store.hh"
#include "serialise.hh"
#include "remote-store.hh"


namespace nix {


boost::shared_ptr<StoreAPI> store;


boost::shared_ptr<StoreAPI> openStore(bool reserveSpace)
{
    if (getEnv("NIX_REMOTE") == "")
        return boost::shared_ptr<StoreAPI>(new LocalStore(reserveSpace));
    else
        return boost::shared_ptr<StoreAPI>(new RemoteStore());
}


}
