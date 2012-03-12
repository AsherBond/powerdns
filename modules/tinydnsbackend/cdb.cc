#include "cdb.hh"
#include <cdb.h>
#include <pdns/misc.hh>
#include <pdns/iputils.hh>
#include <utility>


CDB::CDB(const string &cdbfile)
{
	
	int fd = open(cdbfile.c_str(), O_RDONLY);
	if (fd < 0)
	{
		L<<Logger::Error<<"Failed to open cdb database file '"<<cdbfile<<"'. Error: "<<stringerror()<<endl;
		throw new AhuException(backendname + " Failed to open cdb database file '"+cdbfile+"'. Error: " + stringerror());
	}

	int cdbinit = cdb_init(&d_cdb, fd);
	if (cdbinit < 0) 
	{
		L<<Logger::Error<<"Failed to initialize cdb database. ErrorNr: '"<<cdbinit<<endl;
		throw new AhuException(backendname + " Failed to initialize cdb database.");
	}
}

CDB::~CDB() {
	cdb_free(&d_cdb);
	close(cdb_fileno(&d_cdb));
}

int CDB::searchKey(const string &key) {
	d_searchType = 1;

	// A 'bug' in tinycdb (the lib used for reading the CDB files) means we have to copy the key because the cdb_find struct
	// keeps a pointer to it.
	d_key = strdup(key.c_str());
	return cdb_findinit(&d_cdbf, &d_cdb, d_key, key.size());
}

bool CDB::searchSuffix(const string &key) {
	d_searchType = 2;

	//See CDB::searchKey() 
	d_key = strdup(key.c_str());

	// We are ok wiht a search on things, but we do want to know if a record with that key exists.........
	bool hasDomain = (cdb_find(&d_cdb, key.c_str(), key.size()) == 1);
	if (hasDomain) {
		cdb_seqinit(&d_seqPtr, &d_cdb);
	}

	return hasDomain;
}

void CDB::searchAll() {
	d_searchType = 3;
	cdb_seqinit(&d_seqPtr, &d_cdb);
}

bool CDB::moveToNext() {
	int hasNext = 0;
	switch(d_searchType) {
		case 1:
			hasNext = cdb_findnext(&d_cdbf);
			break;
		case 2:
		case 3:
			hasNext = cdb_seqnext(&d_seqPtr, &d_cdb);
			break;
	}
	return (hasNext > 0);
}

bool CDB::readNext(pair<string, string> &value) {
	while (moveToNext()) {
		unsigned int pos;
		unsigned int len;
		
		pos = cdb_keypos(&d_cdb);
		len = cdb_keylen(&d_cdb);
		
		char *key = (char *)malloc(len);
		cdb_read(&d_cdb, key, len, pos);
		
		if (d_searchType == 2) {
			char *p = strstr(key, d_key);
        	        if (p == NULL) {
                        	free(key);
	                        continue;
        	        }
		}
		string skey(key, len);
		free(key);

		pos = cdb_datapos(&d_cdb);
		len = cdb_datalen(&d_cdb);
		char *val = (char *)malloc(len);
		cdb_read(&d_cdb, val, len, pos);
		string sval(val, len);
		free(val);

		value = make_pair(skey, sval);
		return true;
	}
	// We're done searching, so we can clean up d_key
	if (d_searchType != 3) {
		free(d_key);
	}
	return false;
}


vector<string> CDB::findall(string &key)
{
	vector<string> ret;
	struct cdb_find cdbf;
	
	cdb_findinit(&cdbf, &d_cdb, key.c_str(), key.size());
	int x=0;
	while(cdb_findnext(&cdbf) > 0) {
		x++;
		unsigned int vpos = cdb_datapos(&d_cdb);
		unsigned int vlen = cdb_datalen(&d_cdb);
		char *val = (char *)malloc(vlen);
		cdb_read(&d_cdb, val, vlen, vpos);
		string sval(val, vlen);
		ret.push_back(sval);
		free(val);
	}
	return ret;
}
