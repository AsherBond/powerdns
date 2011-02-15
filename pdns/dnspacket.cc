/*
    PowerDNS Versatile Database Driven Nameserver
    Copyright (C) 2001 - 2011  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2 as 
    published by the Free Software Foundation

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "utility.hh"
#include <cstdio>

#include <cstdlib>
#include <sys/types.h>

#include <iostream>  

#include <string>
#include <errno.h>
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <boost/foreach.hpp>
#include "dnsseckeeper.hh"
#include "dns.hh"
#include "dnsbackend.hh"
#include "ahuexception.hh"
#include "dnspacket.hh"
#include "logger.hh"
#include "arguments.hh"
#include "dnswriter.hh"
#include "dnsparser.hh"
#include "dnsrecords.hh"
#include "dnssecinfra.hh" 

DNSPacket::DNSPacket() 
{
  d_wrapped=false;
  d_compress=true;
  d_tcp=false;
  d_wantsnsid=false;
  d_dnssecOk=false;
}

const string& DNSPacket::getString()
{
  if(!d_wrapped)
    wrapup();

  return d_rawpacket;
}

string DNSPacket::getRemote() const
{
  return remote.toString();
}

uint16_t DNSPacket::getRemotePort() const
{
  return remote.sin4.sin_port;
}

DNSPacket::DNSPacket(const DNSPacket &orig)
{
  DLOG(L<<"DNSPacket copy constructor called!"<<endl);
  d_socket=orig.d_socket;
  remote=orig.remote;
  d_qlen=orig.d_qlen;
  d_dt=orig.d_dt;
  d_compress=orig.d_compress;
  d_tcp=orig.d_tcp;
  qtype=orig.qtype;
  qclass=orig.qclass;
  qdomain=orig.qdomain;
  d_maxreplylen = orig.d_maxreplylen;
  d_ednsping = orig.d_ednsping;
  d_wantsnsid = orig.d_wantsnsid;
  d_dnssecOk = orig.d_dnssecOk;
  d_rrs=orig.d_rrs;
  
  d_tsigkeyname = orig.d_tsigkeyname;
  d_tsigprevious = orig.d_tsigprevious;
  d_tsigtimersonly = orig.d_tsigtimersonly;
  d_trc = orig.d_trc;
  d_tsigsecret = orig.d_tsigsecret;
  
  d_wrapped=orig.d_wrapped;

  d_rawpacket=orig.d_rawpacket;
  d=orig.d;
}

void DNSPacket::setRcode(int v)
{
  d.rcode=v;
}

void DNSPacket::setAnswer(bool b)
{
  if(b) {
    d_rawpacket.assign(12,(char)0);
    memset((void *)&d,0,sizeof(d));
    
    d.qr=b;
  }
}

void DNSPacket::setA(bool b)
{
  d.aa=b;
}

void DNSPacket::setID(uint16_t id)
{
  d.id=id;
}

void DNSPacket::setRA(bool b)
{
  d.ra=b;
}

void DNSPacket::setRD(bool b)
{
  d.rd=b;
}

void DNSPacket::setOpcode(uint16_t opcode)
{
  d.opcode=opcode;
}


void DNSPacket::clearRecords()
{
  d_rrs.clear();
}

void DNSPacket::addRecord(const DNSResourceRecord &rr)
{
  if(d_compress)
    for(vector<DNSResourceRecord>::const_iterator i=d_rrs.begin();i!=d_rrs.end();++i) 
      if(rr.qname==i->qname && rr.qtype==i->qtype && rr.content==i->content) {
        if(rr.qtype.getCode()!=QType::MX && rr.qtype.getCode()!=QType::SRV)
          return;
        if(rr.priority==i->priority)
          return;
      }

  d_rrs.push_back(rr);
}



static int rrcomp(const DNSResourceRecord &A, const DNSResourceRecord &B)
{
  if(A.d_place < B.d_place)
    return 1;

  return 0;
}

vector<DNSResourceRecord*> DNSPacket::getAPRecords()
{
  vector<DNSResourceRecord*> arrs;

  for(vector<DNSResourceRecord>::iterator i=d_rrs.begin();
      i!=d_rrs.end();
      ++i)
    {
      if(i->d_place!=DNSResourceRecord::ADDITIONAL && 
         (i->qtype.getCode()==15 || 
          i->qtype.getCode()==2 )) // CNAME or MX or NS
        {
          arrs.push_back(&*i);
        }
    }

  return arrs;

}

vector<DNSResourceRecord*> DNSPacket::getAnswerRecords()
{
  vector<DNSResourceRecord*> arrs;

  for(vector<DNSResourceRecord>::iterator i=d_rrs.begin();
      i!=d_rrs.end();
      ++i)
    {
      if(i->d_place!=DNSResourceRecord::ADDITIONAL) 
	arrs.push_back(&*i);
    }
  return arrs;
}


void DNSPacket::setCompress(bool compress)
{
  d_compress=compress;
  d_rawpacket.reserve(65000);
  d_rrs.reserve(200);
}

bool DNSPacket::couldBeCached()
{
  return d_ednsping.empty() && !d_wantsnsid && qclass==QClass::IN;
}
#include "base64.hh"
void DNSPacket::addTSIG(DNSPacketWriter& pw)
{
  string toSign;
  uint16_t len = htons(d_tsigprevious.length());
  toSign.append((char*)&len, 2);
  
  toSign.append(d_tsigprevious);
  toSign.append(&*pw.getContent().begin(), &*pw.getContent().end());
  
  //  cerr<<"toSign size now: "<<toSign.size()<<", keyname '"<<d_tsigkeyname<<"', secret "<<Base64Encode(d_tsigsecret)<<endl;

  // now add something that looks a lot like a TSIG record, but isn't
  vector<uint8_t> signVect;
  DNSPacketWriter dw(signVect, "", 0);
  if(!d_tsigtimersonly) {
    dw.xfrLabel(d_tsigkeyname, false);
    dw.xfr16BitInt(0xff); // class
    dw.xfr32BitInt(0);    // TTL
    dw.xfrLabel(d_trc.d_algoName, false);
  }  
  uint32_t now = d_trc.d_time; 
  dw.xfr48BitInt(now);
  dw.xfr16BitInt(d_trc.d_fudge); // fudge
  
  if(!d_tsigtimersonly) {
    dw.xfr16BitInt(d_trc.d_eRcode); // extended rcode
    dw.xfr16BitInt(d_trc.d_otherData.length()); // length of 'other' data
    //    dw.xfrBlob(d_trc.d_otherData);
  }
  
  const vector<uint8_t>& signRecord=dw.getRecordBeingWritten();
  toSign.append(&*signRecord.begin(), &*signRecord.end());

  d_trc.d_mac = calculateMD5HMAC(d_tsigsecret, toSign);
  //  d_trc.d_mac[0]++; // sabotage
  pw.startRecord(d_tsigkeyname, QType::TSIG, 0, 0xff, DNSPacketWriter::ADDITIONAL); 
  d_trc.toPacket(pw);
  pw.commit();
}

/** Must be called before attempting to access getData(). This function stuffs all resource
 *  records found in rrs into the data buffer. It also frees resource records queued for us.
 */
void DNSPacket::wrapup()
{
  if(d_wrapped) {
    return;
  }

  DNSResourceRecord rr;
  vector<DNSResourceRecord>::iterator pos;

  // we now need to order rrs so that the different sections come at the right place
  // we want a stable sort, based on the d_place field

  stable_sort(d_rrs.begin(),d_rrs.end(), rrcomp);
  static bool mustNotShuffle = ::arg().mustDo("no-shuffle");

  if(!d_tcp && !mustNotShuffle) {
    shuffle(d_rrs);
  }
  d_wrapped=true;

  vector<uint8_t> packet;
  DNSPacketWriter pw(packet, qdomain, qtype.getCode(), qclass);

  pw.getHeader()->rcode=d.rcode;
  pw.getHeader()->aa=d.aa;
  pw.getHeader()->ra=d.ra;
  pw.getHeader()->qr=d.qr;
  pw.getHeader()->id=d.id;
  pw.getHeader()->rd=d.rd;

  DNSPacketWriter::optvect_t opts;
  if(d_wantsnsid) {
    opts.push_back(make_pair(3, ::arg()["server-id"]));
  }

  if(!d_ednsping.empty()) {
    opts.push_back(make_pair(4, d_ednsping));
  }

  if(!d_rrs.empty() || !opts.empty()) {
    try {
      for(pos=d_rrs.begin(); pos < d_rrs.end(); ++pos) {
        // this needs to deal with the 'prio' mismatch:
        if(pos->qtype.getCode()==QType::MX || pos->qtype.getCode() == QType::SRV) {  
          pos->content = lexical_cast<string>(pos->priority) + " " + pos->content;
        }

        if(!pos->content.empty() && pos->qtype.getCode()==QType::TXT && pos->content[0]!='"') {
          pos->content="\""+pos->content+"\"";
        }
        if(pos->content.empty())  // empty contents confuse the MOADNS setup
          pos->content=".";
        
        pw.startRecord(pos->qname, pos->qtype.getCode(), pos->ttl, pos->qclass, (DNSPacketWriter::Place)pos->d_place); 
        shared_ptr<DNSRecordContent> drc(DNSRecordContent::mastermake(pos->qtype.getCode(), 1, pos->content)); 
              drc->toPacket(pw);
        if(!d_tcp && pw.size() + 20U > getMaxReplyLen()) { // 20 = room for EDNS0
          pw.rollback();
          if(pos->d_place == DNSResourceRecord::ANSWER) {
            pw.getHeader()->tc=1;
          }
          goto noCommit;
        }
      }

      if(!opts.empty() || d_dnssecOk)
        pw.addOpt(2800, 0, d_dnssecOk ? EDNSOpts::DNSSECOK : 0, opts);

      if(!pw.getHeader()->tc) // protect against double commit from addSignature
        pw.commit();
      noCommit:;
    }
    catch(std::exception& e) {
      L<<Logger::Warning<<"Exception: "<<e.what()<<endl;
      throw;
    }
  }
  
  if(!d_trc.d_algoName.empty())
    addTSIG(pw);
  
  d_rawpacket.assign((char*)&packet[0], packet.size());
}

void DNSPacket::setQuestion(int op, const string &qd, int newqtype)
{
  memset(&d,0,sizeof(d));
  d.id=Utility::random();
  d.rd=d.tc=d.aa=false;
  d.qr=false;
  d.qdcount=1; // is htons'ed later on
  d.ancount=d.arcount=d.nscount=0;
  d.opcode=op;
  qdomain=qd;
  qtype=newqtype;
}

/** convenience function for creating a reply packet from a question packet. Do not forget to delete it after use! */
DNSPacket *DNSPacket::replyPacket() const
{
  DNSPacket *r=new DNSPacket;
  r->setSocket(d_socket);

  r->setRemote(&remote);
  r->setAnswer(true);  // this implies the allocation of the header
  r->setA(true); // and we are authoritative
  r->setRA(0); // no recursion available
  r->setRD(d.rd); // if you wanted to recurse, answer will say you wanted it 
  r->setID(d.id);
  r->setOpcode(d.opcode);

  r->d_dt=d_dt;
  r->d.qdcount=1;
  r->d_tcp = d_tcp;
  r->qdomain = qdomain;
  r->qtype = qtype;
  r->qclass = qclass;
  r->d_maxreplylen = d_maxreplylen;
  r->d_ednsping = d_ednsping;
  r->d_wantsnsid = d_wantsnsid;
  r->d_dnssecOk = d_dnssecOk;
  
  if(!d_tsigkeyname.empty()) {
    r->d_tsigkeyname = d_tsigkeyname;
    r->d_tsigprevious = d_tsigprevious;
    r->d_trc = d_trc;
    r->d_tsigsecret = d_tsigsecret;
    r->d_tsigtimersonly = d_tsigtimersonly;
  }
  return r;
}

void DNSPacket::spoofQuestion(const string &qd)
{
  string label=simpleCompress(qd);
  for(string::size_type i=0;i<label.size();++i)
    d_rawpacket[i+sizeof(d)]=label[i];
  d_wrapped=true; // if we do this, don't later on wrapup
}

int DNSPacket::noparse(const char *mesg, int length)
{
  d_rawpacket.assign(mesg,length); 
  if(length < 12) { 
    L << Logger::Warning << "Ignoring packet: too short from "
      << getRemote() << endl;
    return -1;
  }
  d_wantsnsid=false;
  d_ednsping.clear();
  d_maxreplylen=512;
  memcpy((void *)&d,(const void *)d_rawpacket.c_str(),12);
  return 0;
}

void DNSPacket::setTSIGDetails(const TSIGRecordContent& tr, const string& keyname, const string& secret, const string& previous, bool timersonly)
{
  d_trc=tr;
  d_tsigkeyname = keyname;
  d_tsigsecret = secret;
  d_tsigprevious = previous;
  d_tsigtimersonly=timersonly;
}


bool DNSPacket::getTSIGDetails(TSIGRecordContent* trc, string* keyname, string* message) const
{
  MOADNSParser mdp(d_rawpacket);

  if(!mdp.getTSIGPos()) 
    return false;
  
  bool gotit=false;
  for(MOADNSParser::answers_t::const_iterator i=mdp.d_answers.begin(); i!=mdp.d_answers.end(); ++i) {          
    if(i->first.d_type == QType::TSIG) {
      *trc = *boost::dynamic_pointer_cast<TSIGRecordContent>(i->first.d_content);
      
      gotit=true;
      *keyname = i->first.d_label;
    }
  }
  if(!gotit)
    return false;
  
  //now sign: the packet as it would've been w/o the TSIG
  // the outcome should be the mac we just stripped off.
  if(message) {
    string packet(d_rawpacket);
  
    packet.resize(mdp.getTSIGPos());
    packet[sizeof(struct dnsheader)-1]--;
    message->assign(packet);
  
    vector<uint8_t> signVect;
    DNSPacketWriter dw(signVect, "", 0);
    dw.xfrLabel(*keyname, false);
    dw.xfr16BitInt(0xff); // class
    dw.xfr32BitInt(0);    // TTL
    dw.xfrLabel(trc->d_algoName, false); 
    uint32_t now = trc->d_time; 
    dw.xfr48BitInt(now);
    dw.xfr16BitInt(trc->d_fudge); // fudge
    dw.xfr16BitInt(trc->d_eRcode); // extended rcode
    dw.xfr16BitInt(trc->d_otherData.length()); // length of 'other' data
    //    dw.xfrBlob(trc->d_otherData);
  
    const vector<uint8_t>& signRecord=dw.getRecordBeingWritten();
    message->append(&*signRecord.begin(), &*signRecord.end());
  }
  return true;
}

/** This function takes data from the network, possibly received with recvfrom, and parses
    it into our class. Results of calling this function multiple times on one packet are
    unknown. Returns -1 if the packet cannot be parsed.
*/
int DNSPacket::parse(const char *mesg, int length)
try
{
  d_rawpacket.assign(mesg,length); 

  if(length < 12) { 
    L << Logger::Warning << "Ignoring packet: too short from "
      << getRemote() << endl;
    return -1;
  }

  MOADNSParser mdp(d_rawpacket);
  EDNSOpts edo;

  // ANY OPTION WHICH *MIGHT* BE SET DOWN BELOW SHOULD BE CLEARED FIRST!

  d_wantsnsid=false;
  d_dnssecOk=false;
  d_ednsping.clear();
  d_havetsig = mdp.getTSIGPos();


  if(getEDNSOpts(mdp, &edo)) {
    d_maxreplylen=std::min(edo.d_packetsize, (uint16_t)1680);
//    cerr<<edo.d_Z<<endl;
    if(edo.d_Z & EDNSOpts::DNSSECOK)
      d_dnssecOk=true;

    for(vector<pair<uint16_t, string> >::const_iterator iter = edo.d_options.begin();
        iter != edo.d_options.end(); 
        ++iter) {
      if(iter->first == 3) {// 'EDNS NSID'
        d_wantsnsid=1;
      }
      else if(iter->first == 5) {// 'EDNS PING'
        d_ednsping = iter->second;
      }
      else
        ; // cerr<<"Have an option #"<<iter->first<<endl;
    }
  }
  else  {
    d_maxreplylen=512;
  }

  memcpy((void *)&d,(const void *)d_rawpacket.c_str(),12);
  qdomain=mdp.d_qname;
  if(!qdomain.empty()) // strip dot
    boost::erase_tail(qdomain, 1);

  if(!ntohs(d.qdcount)) {
    if(!d_tcp) {
      L << Logger::Warning << "No question section in packet from " << getRemote() <<", rcode="<<(int)d.rcode<<endl;
      return -1;
    }
  }
  
  qtype=mdp.d_qtype;
  qclass=mdp.d_qclass;
  return 0;
}
catch(std::exception& e) {
  return -1;
}

unsigned int DNSPacket::getMaxReplyLen()
{
  return d_maxreplylen;
}

void DNSPacket::setMaxReplyLen(int bytes)
{
  d_maxreplylen=bytes;
}

//! Use this to set where this packet was received from or should be sent to
void DNSPacket::setRemote(const ComboAddress *s)
{
  remote=*s;
}

void DNSPacket::setSocket(Utility::sock_t sock)
{
  d_socket=sock;
}

void DNSPacket::commitD()
{
  d_rawpacket.replace(0,12,(char *)&d,12); // copy in d
}


bool checkForCorrectTSIG(const DNSPacket* q, DNSBackend* B, string* keyname, string* secret, TSIGRecordContent* trc)
{
  string message;
  
  q->getTSIGDetails(trc, keyname, &message);
  uint64_t now = time(0);
  if(abs(trc->d_time - now) > trc->d_fudge) {
    L<<Logger::Error<<"Packet for '"<<q->qdomain<<"' denied: TSIG timestamp delta "<< abs(trc->d_time - now)<<" bigger than 'fudge' "<<trc->d_fudge<<endl;
    return false;
  }
  
  string secret64;
    
  if(!B->getTSIGKey(*keyname, &trc->d_algoName, &secret64)) {
    L<<Logger::Error<<"Packet for domain '"<<q->qdomain<<"' denied: can't find TSIG key with name '"<<*keyname<<"' and algorithm '"<<trc->d_algoName<<"'"<<endl;
    return false;
  }
  B64Decode(secret64, *secret);
  bool result=calculateMD5HMAC(*secret, message) == trc->d_mac;
  if(!result) {
    L<<Logger::Error<<"Packet for domain '"<<q->qdomain<<"' denied: TSIG signature mismatch using '"<<*keyname<<"' and algorithm '"<<trc->d_algoName<<"'"<<endl;
  }
  return result;
}
