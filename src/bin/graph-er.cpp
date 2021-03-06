/*

This program was created at:  Thu May  7 12:10:40 2015
This program was created by:  Zev N. Kronenberg

Contact: zev.kronenber@gmail.com

Organization: Unviersity of Utah
    School of Medicine
    Salt Lake City, Utah

The MIT License (MIT)

Copyright (c) <2015> <Zev N. Kronenberg>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

#include <string>
#include <iostream>
#include <math.h>
#include <cmath>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

#include "split.h"
#include "fastahack/Fasta.h"
#include "ssw_cpp.h"

// openMP - swing that hammer
#include <omp.h>

// bamtools and my headers
#include "api/BamMultiReader.h"
#include "readPileUp.h"

// gsl header
#include "gauss.h"

using namespace std;
using namespace BamTools;

struct options{
  std::vector<string> targetBams;
  bool statsOnly         ;  
  bool skipGeno          ;
  int nthreads           ;
  string fasta           ;
  string graphOut        ;
  map<string, int> toSkip;
  string seqid           ;
  vector<int> region     ;
  

}globalOpts;

struct regionDat{
  int seqidIndex ;
  int start      ;
  int end        ;
};

struct readPair{
  int          flag;
  int          count;
  BamAlignment al1;
  BamAlignment al2;  
};


struct cigDat{
  int  Length;
  char Type;
};

struct saTag{
  int seqid;
  int pos;
  bool strand;
  vector<cigDat> cig;
};

struct node;
struct edge;

struct edge{
  node * L;
  node * R;
  map<char,int> support;
};

struct node{
  int   seqid          ;
  int   pos            ;
  int   endSupport     ;
  int   beginSupport   ; 
  bool  collapsed      ;
  vector <edge *> eds  ;
};

struct graph{
  map< int, map<int, node *> > nodes;
  vector<edge *>   edges;
}globalGraph;

struct insertDat{
  map<string, double> mus ; // mean of insert length for each indvdual across 1e6 reads
  map<string, double> sds ;  // standard deviation
  map<string, double> lq  ;  // 25% of data
  map<string, double> up  ;  // 75% of the data
  map<string, double> swm ;
  map<string, double> sws ;
  map<string, double> avgD;
  double overallDepth;
} insertDists;

// options

struct breakpoints{
  bool two              ;
  char type             ;
  int seqidIndexL       ;
  int seqidIndexR       ;
  string seqid          ;
  int merged            ;
  int five              ;
  int three             ;
  int svlen             ;
  int totalSupport      ; 
  vector<string> alleles;
  
  vector<vector<double> > genotypeLikelhoods ;
  vector<int>             genotypeIndex      ;
};

static const char *optString = "r:a:g:x:f:e:hsk";

int SangerLookup[126] =    {-1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 0-9     1-10
                            -1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 10-19   11-20
                            -1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 20-29   21-30
                            -1,-1, 0, 1, 2,  3, 4, 5, 6, 7, // 30-39   31-40
			    8, 9,10,11,12, 13,14,15,16,17, // 40-49   41-50
                            18,19,20,21,22, 23,24,25,26,27, // 50-59   51-60
                            28,29,30,31,32, 33,34,35,36,37, // 60-69   61-70
                            38,39,40,41,42, 43,44,45,46,47, // 70-79   71-80
                            48,49,50,51,52, 53,54,55,56,57, // 80-89   81-90
                            58,59,60,61,62, 63,64,65,66,67, // 90-99   91-100
                            68,69,70,71,72, 73,74,75,76,77, // 100-109 101-110
                            78,79,80,81,82, 83,84,85,86,87, // 110-119 111-120
                            88,89,90,91,92, 93           }; // 120-119 121-130

int IlluminaOneThree[126] = {-1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 0-9     1-10
                             -1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 10-19   11-20
                             -1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 20-29   21-30
                             -1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 30-39   31-40
                             -1,-1,-1,-1,-1  -1,-1,-1,-1,-1, // 40-49   41-50
                             -1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 50-59   51-60
                             -1,-1,-1,-1,-1,  0, 1, 2, 3, 4, // 60-69   61-70
			     5, 6, 7, 8, 9, 10,11,12,13,14, // 70-79   71-80
                             15,16,17,18,19, 20,21,22,23,24, // 80-89   81-90
                             25,26,27,28,29, 30,31,32,33,34, // 90-99   91-100
                             35,36,37,38,39, 40,-1,-1,-1,-1, // 100-109 101-110
                             -1,-1,-1,-1,-1, -1,-1,-1,-1,-1, // 110-119 111-120
                             -1,-1,-1,-1,-1, -1,-1        }; // 120-119 121-130

// omp lock

omp_lock_t lock;
omp_lock_t glock;

static void PrintAlignment(const StripedSmithWaterman::Alignment& alignment){
  cerr << "===== SSW result =====" << endl;
  cerr << "Best Smith-Waterman score:\t" << alignment.sw_score << endl
       << "Next-best Smith-Waterman score:\t" << alignment.sw_score_next_best << endl
       << "Reference start:\t" << alignment.ref_begin << endl
       << "Reference end:\t" << alignment.ref_end << endl
       << "Query start:\t" << alignment.query_begin << endl
       << "Query end:\t" << alignment.query_end << endl
       << "Next-best reference end:\t" << alignment.ref_end_next_best << endl
       << "Number of mismatches:\t" << alignment.mismatches << endl
       << "Cigar: " << alignment.cigar_string << endl;
  cerr << "======================" << endl;
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : string reference

 Function does   : revcomp

 Function returns: string

*/
void Comp(string & seq){
  
  locale loc;

  for (size_t i = 0; i < seq.size(); ++i)
    {
      switch (toupper(seq[i], loc))
	{
	case 'A':
	  {
	    seq[i] = 'T';
	    break;
	  }
	case 'T':
	  {
	    seq[i] = 'A';
	    break;
	  }
	case 'G':
	  {
	    seq[i] = 'C';
	    break;
	  }
	case 'C':
	  {
	    seq[i] = 'G';
	    break;
	  }
	default:
	  {
	    seq[i] = 'N';
	    break;
	  }
	}
    } 
}



//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : nothing

 Function does   : prints help

 Function returns: NA

*/

void printVersion(void){
  cerr << "Version: " << VERSION << endl;
  cerr << "Contact: zev.kronenberg [at] gmail.com " << endl;
  cerr << "Notes  : -If you find a bug, please open a report on github!" << endl;
  cerr << endl;
}


void printHelp(void){
//------------------------------- XXXXXXXXXX --------------------------------  
  cerr << " Usage:  " << endl;
  cerr << "       WHAM-GRAPHENING -f my.bam -a my.fasta \\ " << endl;
  cerr << "       -g my.graph.out.txt -e M,GL000207.1 2> wham.err > wham.out" << endl;
  cerr << endl;
  cerr << " Required:  " << endl;
//------------------------------- XXXXXXXXXX --------------------------------  

  cerr << "          -f - <STRING> - A sorted and indexed bam file or a list" << endl;
  cerr << "                          of bams: a.bam,b.bam,..." << endl;
  cerr << "          -a - <STRING> - The reference genome (indexed fasta).  " << endl;
  cerr << endl;
  cerr << " Optional:  " << endl;
  cerr << "          -s - <FLAG>   - Exits the program after the stats are      " << endl;
  cerr << "                          gathered.                                  " << endl;
  cerr << "          -k - <FLAG>   - Skip genotyping (much faster).             " << endl;
  cerr << "          -g - <STRING> - File to write graph to (very large output)." << endl;
  cerr << "          -e - <STRING> - Comma sep. list of seqids to skip.         " << endl;
  cerr << "          -r - <STRING> - Region in format: seqid:start-end          " << endl;
  cerr << "          -x - <INT>    - Number of CPUs to use [default: all cores]." << endl;
  cerr << endl;
  cerr << " Output:  " << endl;
  cerr << "        STDERR: Run statistics and bam stats                        " << endl;    
  cerr << "        STOUT : SV calls in BEDPE format (VCF soon)                 " << endl;  
  cerr << endl;
  printVersion();
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of breakpoint calls

 Function does   : return true if the left is less than the right

 Function returns: bool

*/

bool sortBreak(breakpoints * L, breakpoints * R){
  
  if(L->seqidIndexL == R->seqidIndexL ){
    if(L->five < R->five){
      return true;
    }
  }
  else{
    if(L->seqidIndexL < R->seqidIndexL){
      return true;
    }
  }
  return false;
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of pointers to breakpoints and a bamtools RefVector

 Function does   : prints a bedbe format

 Function returns: nada

*/


void printBEDPE(vector<breakpoints *> & calls, RefVector & seqs){

  int index = 0;

  for(vector<breakpoints *>::iterator c = calls.begin(); c != calls.end(); c++){
    

    index += 1;
   
    stringstream ss;

    string type = "NONE";
    switch((*c)->type){
    case 'D':
      type = "DEL";
      break;
    case 'U':
      type = "DUP";
      break;
    case 'I':
      type = "INR";
      break;
    case 'V':
      type = "INV";
      break;
    default:
      break;
    }
    ss << seqs[(*c)->seqidIndexL].RefName 
       << "\t"
       << ((*c)->five - 10)
       << "\t"
       << ((*c)->five + 10)
       << "\t"
       << seqs[(*c)->seqidIndexL].RefName
       << "\t"
       << ((*c)->three - 10)
       << "\t"
       << ((*c)->three + 10)
       << "\t"
       << type << ":" << index
       << "\t"
       << "."
       << "\t"
       << "."
       << "\t"
       << ".";
    if((*c)->type == 'D' || (*c)->type == 'V' || (*c)->type == 'U' ){
      ss << "\t" << "SVLEN=" << (*c)->svlen << ";"; 
    }
    
    ss << "\tSUPPORT=" << (*c)->totalSupport << ";" << "MERGED=" << (*c)->merged << ";";
    
    for(unsigned int i = 0; i < (*c)->genotypeIndex.size(); i++){
      
      if((*c)->genotypeIndex[i] == 0){
	ss << "\t" << "0/0;" << (*c)->genotypeLikelhoods[i][0] << "," << (*c)->genotypeLikelhoods[i][1] << "," << (*c)->genotypeLikelhoods[i][2] ;
      }
      else if((*c)->genotypeIndex[i] == 1){
	ss << "\t" << "0/1;" << (*c)->genotypeLikelhoods[i][0] << "," << (*c)->genotypeLikelhoods[i][1] << "," << (*c)->genotypeLikelhoods[i][2] ;
      }
      else if((*c)->genotypeIndex[i] == 2){
	ss << "\t" << "1/1;" << (*c)->genotypeLikelhoods[i][0] << "," << (*c)->genotypeLikelhoods[i][1] << "," << (*c)->genotypeLikelhoods[i][2] ;
      }     
      else{
	cerr << "FATAL: printBEDPE: unknown genotype." << endl;
	exit(1);
      }
    }
    ss << endl;
    cout << ss.str();
  }
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of ints

 Function does   : calculates the mean

 Function returns: double

*/

double mean(vector<int> & data){

  double sum = 0;

  for(vector<int>::iterator it = data.begin(); it != data.end(); it++){
    sum += (*it);
  }
  return sum / data.size();
}

double mean(vector<double> & data){

  double sum = 0;

  for(vector<double>::iterator it = data.begin(); it != data.end(); it++){
    sum += (*it);
  }
  return sum / data.size();
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of doubles

 Function does   : calculates the var

 Function returns: double

*/

double var(vector<double> & data, double mu){
  double variance = 0;

  for(vector<double>::iterator it = data.begin(); it != data.end(); it++){
    variance += pow((*it) - mu,2);
  }

  return variance / (data.size() - 1);
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : int pointer, vector cigDat

 Function does   : finds end position

 Function returns: void

*/
void endPos(vector<cigDat> & cigs, int * pos){

  for(vector<cigDat>::iterator it = cigs.begin();
      it != cigs.end(); it++){

    switch( (*it).Type ){
    case 'M':
      {
        *pos += (*it).Length;
        break;
      }
    case 'X':
      {
        *pos += (*it).Length;
        break;
      }
    case 'D':
      {
        *pos += (*it).Length;
        break;
      }
    case '=':
      {
        *pos += (*it).Length;
        break;
      }
    case 'N':
      {
	*pos += (*it).Length;
        break;
      }
    default:
      break;
    }
  }
  // WARNING: this needs to be double checked
   *pos -= 1;
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : node pointer, vector of node pointers

 Function does   : populates a vector for a sub graph

 Function returns: void

*/


void getTree(node * n, vector<node *> & ns){
  
  map<int, int>  seen ;
  vector<edge *> edges;
  
  if(!n->eds.empty()){
    edges.insert(edges.end(), n->eds.begin(), n->eds.end());
  }
  
  seen[n->pos] = 1;

  ns.push_back(n);

  // if something is pushed to the back of the stack it changes the positions ! be warned.

  while(!edges.empty()){
    
    //cerr << " getting graph: left pointer POS: " << edges.back()->L->pos << " right pointer POS: " <<  edges.back()->R->pos << endl;
    

     uint hit = 0;
     
     if(seen.find(edges.back()->L->pos) != seen.end() && seen.find(edges.back()->R->pos) != seen.end() ){
       hit = 1;
     }
     else if(seen.find(edges.back()->L->pos) == seen.end() && seen.find(edges.back()->R->pos) == seen.end()){
       seen[edges.back()->L->pos] = 1;
       seen[edges.back()->R->pos] = 1;
       ns.push_back(edges.back()->L);
       ns.push_back(edges.back()->R);
       edges.insert(edges.end(), edges.back()->L->eds.begin(), edges.back()->L->eds.end());       
       edges.insert(edges.end(), edges.back()->R->eds.begin(), edges.back()->R->eds.end());
     }
     else if(seen.find(edges.back()->L->pos) == seen.end()){
       
       seen[edges.back()->L->pos] = 1;

       ns.push_back(edges.back()->L);
       
       edges.insert(edges.end(), edges.back()->L->eds.begin(), edges.back()->L->eds.end());
       
     }
     else{
       seen[edges.back()->R->pos] = 1;
       ns.push_back(edges.back()->R);

       edges.insert(edges.end(), edges.back()->R->eds.begin(), edges.back()->R->eds.end());
     }

    if(hit == 1){
      edges.pop_back();
    }
  }
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : breakpoints *, RefSeq

 Function does   : provides ref and alt

 Function returns: bool

*/

bool genAlleles(breakpoints * bp, FastaReference & rs, RefVector & rv){
    
  string ref ;
  string alt ;

  if(bp->type == 'D'){   

    if((bp->five - 500) < 0){
      return false;
    }
    if((bp->three + 500) > rv[bp->seqidIndexL].RefLength){
      return false;
    }

    bp->seqid = rv[bp->seqidIndexL].RefName;
    ref = rs.getSubSequence(rv[bp->seqidIndexL].RefName, bp->five - 200 +1, bp->svlen + 400 );
    alt = rs.getSubSequence(rv[bp->seqidIndexL].RefName, bp->five - 200 +1, 200) +
      rs.getSubSequence(rv[bp->seqidIndexL].RefName, bp->three, 200);

    //duplication;

  }
  if(bp->type == 'U'){
    if((bp->five - 500) < 0){
      return false;
    }
    if((bp->three + 500) > rv[bp->seqidIndexL].RefLength){
      return false;
    }
    ref = rs.getSubSequence(rv[bp->seqidIndexL].RefName, bp->five - 200 +1, bp->svlen + 400 );
    alt = rs.getSubSequence(rv[bp->seqidIndexL].RefName, bp->five - 200 +1, + bp->svlen +200)
      +rs.getSubSequence(rv[bp->seqidIndexL].RefName, bp->five + 1, bp->svlen)
      +rs.getSubSequence(rv[bp->seqidIndexL].RefName, bp->three  +1, 200);
  }
  if(bp->type == 'V'){

    if((bp->five - 500) < 0){
      return false;
    }
    if((bp->three + 500) > rv[bp->seqidIndexL].RefLength){
      return false;
    }
    ref = rs.getSubSequence(rv[bp->seqidIndexL].RefName, bp->five - 200 +1, bp->svlen + 400 );    
    string inv = ref.substr(200, bp->svlen );
    inv = string(inv.rbegin(), inv.rend());
    Comp(inv);
    
    //    cerr << "INV allele: " << rv[bp->seqidIndexL].RefName << " " << bp->five << " " << bp->three << " " << bp->svlen << endl;
    //    cerr << inv << endl ;
    //    cerr << ref << endl << endl;

    alt = ref.substr(0,200) + inv + ref.substr(ref.size() - 200, 200);
  }
    
  if(ref.size() > 800){
    ref = ref.substr(0,400) + ref.substr(ref.size()-400,  400);
  }
  if(alt.size() > 800){
    alt = alt.substr(0,400) + alt.substr(alt.size() -400, 400);
  }

  bp->alleles.push_back(ref) ;
  bp->alleles.push_back(alt) ;

  return true;
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : edge pointer 

 Function does   : init

 Function returns: void

*/

void initEdge(edge * e){
  e->L = NULL;
  e->R = NULL;


  e->support['L'] = 0;
  e->support['H'] = 0;
  e->support['S'] = 0;
  e->support['I'] = 0;
  e->support['D'] = 0;
  e->support['V'] = 0;
  e->support['M'] = 0;
  e->support['R'] = 0;
  e->support['X'] = 0;

}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of doubles and a seperator

 Function does   : joins vector with separator

 Function returns: string

*/

string join(vector<double> & ints, string sep){

  stringstream ss;

  for(vector<double>::iterator sit = ints.begin(); sit != ints.end(); sit++){
    ss << *sit << sep;
  }
  return ss.str();
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of ints and separator

 Function does   : joins vector with separator

 Function returns: string

*/

string join(vector<int> & ints, string sep){

  stringstream ss;

  for(vector<int>::iterator sit = ints.begin(); sit != ints.end(); sit++){
    ss << *sit << sep;
  }
  return ss.str();
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of strings and separator

 Function does   : joins vector with separator

 Function returns: string

*/

string join(vector<string> & strings, string sep){

  string joined = "";

  for(vector<string>::iterator sit = strings.begin(); sit != strings.end(); 
      sit++){
    joined = joined + sep + (*sit) ;
  }
  return joined;
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of strings

 Function does   : joins vector with returns;

 Function returns: string

*/

string joinReturn(vector<string> strings){

  string joined = "";

  for(vector<string>::iterator sit = strings.begin(); sit != strings.end(); 
      sit++){
    joined = joined + " " + (*sit) + "\n";
  }
  return joined;
}



//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : read pair pointer

 Function does   : -->s s<-- tests if pairs suggests insertion

 Function returns: bool

*/

bool isPointIn(readPair * rp){

  if(!rp->al1.IsMapped() || !rp->al2.IsMapped()){
    return false;
  }
  if(rp->al1.RefID != rp->al2.RefID){
    return false;
  }
  if(rp->al1.Position <= rp->al2.Position){
    
    if(rp->al1.CigarData.back().Type == 'S' && 
       rp->al2.CigarData.front().Type == 'S'){
      return true;
    }
  }
  else{
    if(rp->al1.CigarData.front().Type == 'S' && 
       rp->al2.CigarData.back().Type == 'S'){
      return true;
    }
  }
  return false;
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : refID (int), left position (int), graph

 Function does   : determine if a node is in the graph

 Function returns: bool

*/


bool isInGraph(int refID, int pos, graph & lc){

  if(lc.nodes.find(refID) == lc.nodes.end()){
    return false;
  }
 
  if(lc.nodes[refID].find(pos) != lc.nodes[refID].end()){
    return true;
  }
  return false;
}



//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : left position, right position, local graph

 Function does   : adds nodes or builds connections

 Function returns: void

*/

void addIndelToGraph(int refID, int l, int r, char s){

  omp_set_lock(&glock);

  if( ! isInGraph(refID, l, globalGraph) 
      &&  ! isInGraph(refID, r, globalGraph) ){
    
    //cerr << "addIndelToGraph: neither node found" << endl;

    node * nodeL;
    node * nodeR;
    edge * ed   ;

    nodeL = new node;
    nodeR = new node;
    ed    = new edge;

    nodeL->collapsed = false;
    nodeR->collapsed = false;
    nodeL->beginSupport = 0;
    nodeL->endSupport   = 0;

    nodeR->beginSupport = 0;
    nodeR->endSupport   = 0;

    initEdge(ed);

    ed->support[s] +=1;


    ed->L = nodeL;
    ed->R = nodeR;

    nodeL->eds.push_back(ed);
    nodeR->eds.push_back(ed);

    nodeL->pos = l;
    nodeL->seqid = refID;
    nodeR->pos = r;
    nodeR->seqid = refID;

    globalGraph.nodes[refID][l] = nodeL;
    globalGraph.nodes[refID][r] = nodeR;

  }
 else if(isInGraph(refID, l, globalGraph) 
	 &&  ! isInGraph(refID, r, globalGraph)){

   //cerr << "addIndelToGraph: left node found" << endl;

   node * nodeR;
   edge * ed;

   nodeR = new node;
   ed    = new edge;

   nodeR->collapsed = false;
   
   nodeR->beginSupport = 0;
   nodeR->endSupport = 0;

   initEdge(ed);
   ed->support[s] += 1;

   nodeR->pos   = r;
   nodeR->seqid = refID; 
   ed->L = globalGraph.nodes[refID][l];
   ed->R = nodeR;
   
   nodeR->eds.push_back(ed);

   globalGraph.nodes[refID][l]->eds.push_back(ed);
   globalGraph.nodes[refID][r] = nodeR;

 }
 else if(! isInGraph(refID, l, globalGraph) 
	 &&  isInGraph(refID, r, globalGraph)){

   //cerr << "addIndelToGraph: right node found" << endl;
   
   node * nodeL;
   edge * ed;

   nodeL = new node;
   ed    = new edge;

   nodeL->collapsed = false;
   nodeL->beginSupport = 0;
   nodeL->endSupport   = 0;


   initEdge(ed);
   ed->support[s] +=1;
   nodeL->pos   = l;
   nodeL->seqid = refID;
   ed->R = globalGraph.nodes[refID][r];
   ed->L = nodeL;

   //   cerr << "LPD RP: " << ed->R->pos << endl;
   
   nodeL->eds.push_back(ed);

   globalGraph.nodes[refID][r]->eds.push_back(ed);
   globalGraph.nodes[refID][l] = nodeL;  
}
 else{
   uint hit = 0;

   for(vector<edge *>::iterator ite 
	 = globalGraph.nodes[refID][l]->eds.begin();
       ite != globalGraph.nodes[refID][l]->eds.end(); ite++){
     if((*ite)->L->pos == l && (*ite)->R->pos == r){
       
       (*ite)->support[s] += 1;

       hit = 1;
     }
   }
   if(hit == 0){
     edge * ne;
     ne = new edge;
     initEdge(ne);
     ne->support[s]+=1;
     ne->L =      globalGraph.nodes[refID][l];
     ne->R =      globalGraph.nodes[refID][r];
     globalGraph.nodes[refID][l]->eds.push_back(ne);
     globalGraph.nodes[refID][r]->eds.push_back(ne);
   }
 }
  omp_unset_lock(&glock);
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : bam alignment, two ints.

 Function does   : finds the positions of indels

 Function returns: string

*/

bool indelToGraph(BamAlignment & ba){

  if(!ba.IsMapped()){
    return false;
  }

  if(ba.MapQuality < 30){
    return false;
  }

  bool hit = false;

  int p = ba.Position;

  for(vector<CigarOp>::iterator ci = ba.CigarData.begin();
      ci != ba.CigarData.end(); ci++){
    
    switch(ci->Type){
    case 'M':
      {
	p += ci->Length;
	break;
      }
    case '=':
      {
        p += ci->Length;
        break;
      }
    case 'N':
      {
        p += ci->Length;
        break;
      }
    case 'X':
      {
	p += ci->Length;
	break;
      }
    case 'I':
      {
	hit = true;
	addIndelToGraph(ba.RefID, p, p + ci->Length, 'I');
	break;
      }
    case 'D':
      {
	hit = true;
	addIndelToGraph(ba.RefID, p , (p + ci->Length ), 'D');
	p += ci->Length;
	break;
      }
    default :
      {
	break;
      }
    }
  }
  return hit;
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of BamTools::CigarOp

 Function does   : joins vector " ";

 Function returns: string

*/

string joinCig(vector<CigarOp> strings){

  stringstream joined ;

  for(vector<CigarOp>::iterator sit = strings.begin(); 
      sit != strings.end(); sit++){
    joined  << (*sit).Length << (*sit).Type; 
  }
  return joined.str();
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of BamTools::CigarOp, int length

 Function does   : looks for clips greater than or equal to length

 Function returns: bool

*/

bool areBothClipped(vector<CigarOp> & ci){

  if(ci.front().Type == 'S' && ci.back().Type == 'S'){
    return true;
  }
  return false;
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of BamTools::CigarOp, int length

 Function does   : looks for clips greater than or equal to length

 Function returns: bool

*/

bool IsLongClip(vector<CigarOp> & ci, unsigned int len){

  if(ci.front().Type == 'S' && ci.front().Length >= len){
    return true;
  }
  if(ci.back().Type == 'S' && ci.back().Length >= len){
    return true;
  }
  return false;
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector<CigarOp>

 Function does   : calculates the number of matching bases 

 Function returns: unsigned int, total number of matching bases

*/

int match(vector<CigarOp> co){
  
  int m = 0;
  
  for(vector<CigarOp>::iterator it = co.begin(); 
      it != co.end(); it++){
    if(it->Type == 'M'){
      m += it->Length;
    }
  }
  
  return m;
  
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : read pair to filter

 Function does   : fails poor quality or non-sv read pairs

 Function returns: true = bad, false = good;

*/

bool pairFailed(readPair * rp){
  
  if(rp->al1.IsMapped() && rp->al2.IsMapped()){
    if(rp->al1.Length == rp->al1.CigarData[0].Length 
       && rp->al1.CigarData[0].Type == 'M' &&
       rp->al2.Length == rp->al2.CigarData[0].Length 
       && rp->al2.CigarData[0].Type == 'M' ){
      return true;
    }
    if(rp->al1.MapQuality < 20 && rp->al2.MapQuality < 20){
      return true;
    }
    if((match(rp->al1.CigarData) + match(rp->al2.CigarData)) < 100){
      return true;
    }
  }
  return false;
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector<cigDat>, string

 Function does   : loads cigar data into a string

 Function returns: NA

*/

void parseCigar(vector<cigDat> & parsedCigar, string cigar){

  unsigned int spot = 0;

  for(unsigned int i = 0; i < cigar.size(); i++){
    if(int(cigar[i] > 57)){
      cigDat tup;
      tup.Length = atoi(cigar.substr(spot, i-spot).c_str());
      tup.Type  = cigar[i];
      parsedCigar.push_back(tup);
    }
  }
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : SA tag string, and vector of saTag

 Function does   : parses string and converts positions to BAM

 Function returns: NA

*/

void parseSA(vector<saTag> & parsed, string tag, map<string, int> & il){

  vector<string> sas = split(tag, ';');

  for(unsigned int i = 0 ; i < sas.size() -1 ; i++){
    
    saTag sDat;
    
    vector<string> sat = split (sas[i], ',');

    if(sat.size() != 6){
      cerr << "FATAL: failure to parse SA optional tag" << endl;
      exit(1);
    }

    if(sat[2].compare("-") == 0){
      sDat.strand = true;
    }
    else{
      sDat.strand = false;
    }

    sDat.seqid = il[sat[0]];
    sDat.pos   = atoi(sat[1].c_str()) - 1;
    parseCigar(sDat.cig, sat[3]);
    parsed.push_back(sDat);

  }
  
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : bam alignment, vector<saTag> 

 Function does   : finds links for splitter to put in graph

 Function returns: NA

*/

void splitToGraph(BamAlignment & al, vector<saTag> & sa){

  if(!al.IsMapped()){
    return;
  }

  if(sa.size() > 1){
    return;
  }

  if(sa[0].seqid != al.RefID){
    return;
  }

  char support = 'S';

  if((sa[0].strand && ! al.IsReverseStrand()) || (! sa[0].strand && al.IsReverseStrand() )){
    support = 'V';
  }

  if(sa.front().cig.front().Type == 'S' 
     && sa.front().cig.back().Type == 'S'){
    return;
  }

  if(al.CigarData.front().Type == 'S' 
     && al.CigarData.back().Type == 'S'){
    return;
  }

  if(al.CigarData.front().Type == 'S'){
    
    int start = al.Position; 
    int end   = sa.front().pos  ;

    if(sa.front().cig.back().Type == 'S'){
      endPos(sa[0].cig, &end) ;
    
      if(end > start ){
	support = 'X';
      }
    }
    
    if(start > end){
      int tmp = start;
      start = end;
      end   = tmp;
    }
    addIndelToGraph(al.RefID, start, end, support);
  }
  else{
    int start = al.GetEndPosition(false,true);
    int end   = sa.front().pos                ;
    if(sa[0].cig.back().Type == 'S'){
      endPos(sa.front().cig, &end);      
    }
    else{
      if(start > end){
	support = 'X';
      }
    }
    if(start > end){      
      start = sa[0].pos;
      end   = al.GetEndPosition(false,true);
    }
    addIndelToGraph(al.RefID, start, end, support);
  }
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : pointer to readPair;

 Function does   : adds high and low insert sizes to graph. both pairs are 
                   treated as mapped

 Function returns: NA

*/

void deviantInsertSize(readPair * rp, char supportType){

  if(! IsLongClip(rp->al1.CigarData, 1) 
     && ! IsLongClip(rp->al2.CigarData, 1)){
    return;
  }
  if(rp->al1.CigarData.front().Type == 'S' 
     || rp->al1.CigarData.back().Type == 'S'){
    int start = rp->al1.Position;
    int end   = rp->al2.Position;
    if(rp->al2.CigarData.back().Type == 'S'){
      end = rp->al2.GetEndPosition();
    }

    if(rp->al1.CigarData.back().Type == 'S'){
      start = rp->al1.GetEndPosition(false,true);
    }
    if(start > end){
      int tmp = end;
      end = start  ;
      start = tmp  ;
    }
    addIndelToGraph(rp->al1.RefID, start, end, supportType);       
  }
  else{
    int start = rp->al2.Position;
    int end   = rp->al1.Position;

    if(rp->al1.CigarData.back().Type == 'S'){
      end = rp->al1.GetEndPosition();
    }

    if(rp->al2.CigarData.back().Type == 'S'){
      start = rp->al2.GetEndPosition(false,true);
    }    
    if(start > end){
      int tmp = end;
      end = start  ;
      start = tmp  ;
    }
   addIndelToGraph(rp->al2.RefID, start, end, supportType);
    
  }

}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : pointer to readPair ; seqid to index

 Function does   : processes Pair

 Function returns: NA

*/

void processPair(readPair * rp, map<string, int> & il, 
		 double * low, double * high){

#ifdef DEBUG
  cerr << "processing pair" << endl;
#endif 

  
  string sa1;
  string sa2;
  
  bool sameStrand = false;

  if(pairFailed(rp)){

#ifdef DEBUG
    cerr << "pair failed filter: " << rp->al1.Name  << endl;
#endif 
    return;
  }
  
  if(rp->al1.RefID != rp->al2.RefID){
    return;
  }
  //  cerr << joinCig(rp->al1.CigarData) << endl;
  indelToGraph(rp->al1);
  //  cerr << joinCig(rp->al2.CigarData) << endl;
  indelToGraph(rp->al2);
  
  if( rp->al1.IsMapped() && rp->al2.IsMapped() ){
    if( ! IsLongClip(rp->al1.CigarData, 10) 
	&& ! IsLongClip(rp->al2.CigarData, 10)){
      return;
    }
    
    if((rp->al1.IsReverseStrand() && rp->al2.IsReverseStrand())
       || (! rp->al1.IsReverseStrand() && ! rp->al2.IsReverseStrand()) ){
      sameStrand = true;
    }
    
    
    if( abs(rp->al1.InsertSize) > *high){
      //      cerr << rp->al1.Name << " H " << rp->al1.InsertSize << endl;
      if(sameStrand){
	deviantInsertSize(rp, 'M'); 
      }
      else{
	deviantInsertSize(rp, 'H'); 
      }
    }    
    if( abs(rp->al1.InsertSize) < *low ){
      //      cerr << rp->al1.Name << " L " << rp->al1.InsertSize << endl;
      if(sameStrand){
	deviantInsertSize(rp, 'R');
      }
      else{
	deviantInsertSize(rp, 'L');
      }
    }
  }           
  if(rp->al1.GetTag("SA", sa1)){
    vector<saTag> parsedSa1;
    parseSA(parsedSa1, sa1, il);
    //    cerr << sa1 << endl;
    splitToGraph(rp->al1, parsedSa1);
    //    cerr << "s1 processed " << endl;
  }
  if(rp->al2.GetTag("SA", sa2)){
    vector<saTag> parsedSa2;
    parseSA(parsedSa2, sa2, il);
    //    cerr << sa2 << endl;
    splitToGraph(rp->al2, parsedSa2);
    //    cerr << "s2 processed " << endl;
  }
  
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : filename, seqid, start, end, refSeqs (bamtools object),
                   paired end store. 

 Function does   : Matches paired ends so they can be processed together.
                   The global pair store is loaded for the missing mates

 Function returns: NA

*/

bool runRegion(string filename,
	       int seqidIndex,
               int start,
               int end,
	       vector< RefData > seqNames,
	       map<string, readPair *> & globalPairStore,
	       map<string, int> & seqInverseLookup){



#ifdef DEBUG
  cerr << "running region: " << seqidIndex << ":" << start << "-" << end << endl;
#endif 

  map<string, int> localInverseLookUp;

  omp_set_lock(&lock);
  for(map<string, int>::iterator itt = seqInverseLookup.begin();
      itt !=  seqInverseLookup.end(); itt++){    
    localInverseLookUp[itt->first] = itt->second;
  }
  insertDat localDists  = insertDists;
  omp_unset_lock(&lock);

  // local graph;
  graph localGraph;

  // local read pair store
  
  double high = localDists.mus[filename] + (2.5 * localDists.sds[filename]);
  double low  = localDists.mus[filename] - (2.5 * localDists.sds[filename]);

  if(low < 0){
    low = 100;
  }

  map<string, readPair *>pairStore;

  BamReader br;
  br.Open(filename);

  if(! br.LocateIndex()){
    vector<string> fileName = split(filename, ".");
    fileName.back() = "bai";
    string indexName = join(fileName, ".");
    if(! br.OpenIndex(indexName) ){
      cerr << "FATAL: cannot find bam index." << endl;
    }
  }
  if(!br.SetRegion(seqidIndex, start, seqidIndex, end)){
    return false;
  }

  BamAlignment al;

  while(br.GetNextAlignmentCore(al)){
    if((al.AlignmentFlag & 0x0800) != 0 ){
      continue;
    }
    if(! al.IsPaired()){
      continue;
    }
    
    if(! al.IsMapped() && ! al.IsMateMapped()){
      continue;
    }
    if(al.IsDuplicate()){
      continue;
    }
    if(! al.IsPrimaryAlignment()){
      continue;
    }

    // dna and read name
    al.BuildCharData();

    if(pairStore.find(al.Name) != pairStore.end()){
      pairStore[al.Name]->count += 1;
      if(pairStore[al.Name]->flag == 2){
	pairStore[al.Name]->al1 = al;
      }
      else{
	pairStore[al.Name]->al2 = al;
      }
      processPair(pairStore[al.Name], localInverseLookUp, &low, &high);
      delete pairStore[al.Name];
      pairStore.erase(al.Name);	 
    }
    else{
      readPair * rp;
      rp = new readPair;
      rp->count = 1;
      if(al.IsFirstMate()){
	rp->flag = 1 ;
	rp->al1  = al;
      }
      else{
	rp->flag = 2 ;
	rp->al2  = al;
      } 
      pairStore[al.Name] = rp;
    }
  }

  // close the bam
  br.Close();

  // load lonely reads into the global struct;
  // if it finds a mate in the global store it processes and deletes
  omp_set_lock(&lock);

  for(map<string, readPair *>::iterator rps = pairStore.begin();
      rps != pairStore.end(); rps++){

    if(globalPairStore.find(rps->first) != globalPairStore.end()){
      globalPairStore[rps->first]->count += 1;
      
      if(globalPairStore[rps->first]->flag == 1 && (*rps->second).flag == 1){
	continue;
      }
      if(globalPairStore[rps->first]->flag == 2 && (*rps->second).flag == 2){
	continue;
      }

      if(globalPairStore[rps->first]->flag == 1){
//	cerr << "c: "     << globalPairStore[rps->first]->count << endl;
//	cerr << "f: "     << globalPairStore[rps->first]->flag << endl;
//	cerr << "test1A " << (*rps->second).al2.Name << endl;
//	cerr << "test1B " << (*rps->second).al1.Name << endl;
	(*globalPairStore[rps->first]).al2 = (*rps->second).al2;
      }
      else{
//	cerr << "c: "     << globalPairStore[rps->first]->count << endl;
//	cerr << "f: "     << globalPairStore[rps->first]->flag << endl;
//	cerr << "test2A " << (*rps->second).al1.Name << endl;
//	cerr << "test2B " << (*rps->second).al2.Name << endl;
      	(*globalPairStore[rps->first]).al1 = (*rps->second).al1;
      }
//      cerr << "INFO: about to process read pairs in different regions: " << globalPairStore[rps->first]->flag << " " 
//	   << globalPairStore[rps->first]->al1.Name      << " "
//	   << globalPairStore[rps->first]->al1.AlignmentFlag      << " "
//	   << globalPairStore[rps->first]->al1.RefID     << " " 
//	   << globalPairStore[rps->first]->al1.Position  << " " 
//	   << globalPairStore[rps->first]->al2.Name      << " " 
//	   << globalPairStore[rps->first]->al2.AlignmentFlag      << " "
//	   << globalPairStore[rps->first]->al2.RefID     << " " 
//	   << globalPairStore[rps->first]->al2.Position  << endl;
      processPair(globalPairStore[rps->first], localInverseLookUp, &low, &high);
      delete globalPairStore[rps->first];
      delete pairStore[rps->first];
      globalPairStore.erase(rps->first);
    }
    else{
      globalPairStore[rps->first] = pairStore[rps->first];
    }
  }

  omp_unset_lock(&lock);
  return true;
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : string

 Function does   : reads bam into graph

 Function returns: NA

*/

void loadBam(string & bamFile){

  bool region    = false;
  int start      = 0;
  int end        = 0;
  string regionSID;

  omp_set_lock(&lock);
  
  if(!globalOpts.seqid.empty()){
    region = true;
    regionSID = globalOpts.seqid;
    start = globalOpts.region.front();
    end   = globalOpts.region.back();
  }

  omp_unset_lock(&lock);

  cerr << "INFO: reading bam file: " << bamFile << endl;

  BamReader br;
 
  if(! br.Open(bamFile)){
    cerr << "\n" << "FATAL: could not open: " << bamFile << endl;
    exit(1);
  }
  if(! br.LocateIndex()){    
    vector<string> fileName = split(bamFile, ".");
    fileName.back() = "bai";
    string indexName = join(fileName, ".");

    cerr << "INFO: Did not find *bam.bai" << endl;
    cerr << "INFO: Trying: " << indexName << endl;
    
    if(! br.OpenIndex(indexName) ){
      cerr << "FATAL: cannot find bam index." << endl;
    }
  }
  
  // grabbing the header
  SamHeader SH = br.GetHeader();

  // if the bam is not sorted die
  if(!SH.HasSortOrder()){
    cerr << "FATAL: sorted bams must have the @HD SO: tag in each SAM header: " << bamFile  << endl;
    exit(1);
  }

  RefVector sequences = br.GetReferenceData();

  //chunking up the genome
  
  vector< regionDat* > regions;

  // inverse lookup for split reads 
  map<string,int> seqIndexLookup;
  
  int seqidIndex = 0;
  for(vector< RefData >::iterator sit = sequences.begin(); sit != sequences.end(); sit++){
      seqIndexLookup[sequences[seqidIndex].RefName] = seqidIndex;
      seqidIndex += 1;
  }
  if(region){
    
      
    regionDat * regionInfo = new regionDat;
    regionInfo->seqidIndex = seqIndexLookup[regionSID];
    regionInfo->start      = start            ;
    regionInfo->end        = end              ; 
    regions.push_back(regionInfo);   

  }
  else{
    
    seqidIndex = 0;
    
    for(vector< RefData >::iterator sit = sequences.begin(); sit != sequences.end(); sit++){
      int start = 0;
      
      if(globalOpts.toSkip.find( (*sit).RefName ) == globalOpts.toSkip.end() && ((*sit).RefLength > 1000)){
	for(;start < (*sit).RefLength ; start += 1000000){
	  regionDat * chunk = new regionDat;
	  
	  chunk->seqidIndex = seqidIndex;
	  chunk->start      = start;
	  chunk->end        = start + 1000000 ;
	  regions.push_back(chunk);
	}
	regionDat * lastChunk = new regionDat;
	lastChunk->seqidIndex = seqidIndex;
	lastChunk->start = start;
	lastChunk->end   = (*sit).RefLength;
	seqidIndex += 1;
	if(start < (*sit).RefLength){
	  regions.push_back(lastChunk);
	}
      }
      else{
	cerr << "INFO: skipping: " << (*sit).RefName << endl;
	seqidIndex += 1;
      }
    }
  }
  // closing the bam reader before running regions
  br.Close();
  
  // global read pair store

  map<string, readPair*> pairStore;



  int Mb = 0;

  // running the regions with openMP
#pragma omp parallel for schedule(dynamic, 3)
 
  for(unsigned int re = 0; re < regions.size(); re++){
    if(! runRegion(bamFile, 
		   regions[re]->seqidIndex, 
		   regions[re]->start, 
		   regions[re]->end, 
		   sequences,
		   pairStore,
		   seqIndexLookup)){
      omp_set_lock(&lock);
      cerr << "WARNING: region failed to run properly: "
           << sequences[regions[re]->seqidIndex].RefName
           << ":"  << regions[re]->start << "-"
           << regions[re]->end
           <<  endl;
      omp_unset_lock(&lock);
    }
    else{
      omp_set_lock(&lock);
      Mb += 1;
      if((Mb % 10) == 0 ){
	cerr << "INFO: " << bamFile
	     << ": processed "
	     << Mb << "Mb of the genome." << endl;	
      }
      omp_unset_lock(&lock);
    }
  }
  cerr << "INFO: " << bamFile << " had " 
       << pairStore.size() 
       << " reads that were not processed" 
       << endl; 
}
//-------------------------------   OPTIONS   --------------------------------
int parseOpts(int argc, char** argv)
    {
    int opt = 0;
    opt = getopt(argc, argv, optString);
    while(opt != -1){
      switch(opt){
      case 'k':
	{
	  globalOpts.skipGeno = true;
	  break;
	}
      case 's':
	{
	  globalOpts.statsOnly = true;
	  break;
	}
      case 'g':
	{
	  globalOpts.graphOut = optarg;
	  cerr << "INFO: graphs will be written to: " <<  globalOpts.graphOut 
	       << endl;
	  break;
	}
      case 'a':
	{
	  globalOpts.fasta = optarg;
	  cerr << "INFO: fasta file: " << globalOpts.fasta << endl;
	  break;
	}
      case 'e':
	{
	  vector<string> seqidsToSkip = split(optarg, ",");
	  for(unsigned int i = 0; i < seqidsToSkip.size(); i++){
	    globalOpts.toSkip[seqidsToSkip[i]] = 1;
	    cerr << "INFO: WHAM will skip seqid: " << seqidsToSkip[i] << endl;
	  }
	  break;
	}
      case 'f':
	{
	  globalOpts.targetBams     = split(optarg, ",");
	  cerr << "INFO: target bams:\n" << joinReturn(globalOpts.targetBams) ;
	  break;
	}
      case 'h':
	{
	  printHelp();
	  exit(1);
	  break;
	}
      case '?':
	{
	  break;
	}
      
      case 'x':
	{
	  globalOpts.nthreads = atoi(((string)optarg).c_str());
	  cerr << "INFO: OpenMP will roughly use " << globalOpts.nthreads 
	       << " threads" << endl;
	  break;
	}
      case 'r':
	{
	  vector<string> tmp_region = split(optarg, ":");
	  if(tmp_region.size() != 2 || tmp_region[1].empty() || tmp_region[0].empty()){
	    cerr << "FATAL: region was not set correctly" << endl;
	    cerr << "INFO:  region format: seqid:start-end" << endl;
	    exit(1);
	  }
	  
	  vector<string> start_end = split(tmp_region[1], "-");
	  globalOpts.seqid = tmp_region[0];
	  globalOpts.region.push_back(atoi(start_end[0].c_str()));
	  globalOpts.region.push_back(atoi(start_end[1].c_str()));

	  if(start_end.size() !=2 || start_end[0].empty() || start_end[1].empty()){
	    cerr << "FATAL: region was not set correctly" << endl;
	    cerr << "INFO:  region format: seqid:start-end" << endl;
	    exit(1);
	  }
	  cerr << "INFO: region set to: " <<   globalOpts.seqid << ":" <<   globalOpts.region[0] << "-" <<  globalOpts.region[1] << endl;

	  if(globalOpts.region.size() != 2){
	    cerr << "FATAL: incorrectly formatted region." << endl;
	    cerr << "FATAL: wham is now exiting."          << endl;
	    exit(1);
	  }
	  break;
	}

      }
      opt = getopt( argc, argv, optString ); 
    }
    return 1;
    }

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector<nodes *>

 Function does   : dumps a graph in dot format

 Function returns: string

*/

string dotviz(vector<node *> & ns){

  stringstream ss;

  ss << "graph {\n";

  for(vector<node *>::iterator it = ns.begin(); 
      it != ns.end(); it++){
    for(vector<edge *>:: iterator iz = (*it)->eds.begin(); 
	iz != (*it)->eds.end(); iz++){



      if((*iz)->support['X'] > 0){
        if((*it)->pos != (*iz)->L->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [style=dashed,penwidth=" << (*iz)->support['X'] << "];\n";
        }
        if((*it)->pos != (*iz)->R->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [style=dashed,penwidth=" << (*iz)->support['X'] << "];\n";
        }
      }




      
      if((*iz)->support['R'] > 0){
	if((*it)->pos != (*iz)->L->pos){
	  ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [color=yellow,penwidth=" << (*iz)->support['R'] << "];\n";
	}
	if((*it)->pos != (*iz)->R->pos){
	  ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [color=yellow,penwidth=" << (*iz)->support['R'] << "];\n";
	}
      }      
      
      if((*iz)->support['M'] > 0){
        if((*it)->pos != (*iz)->L->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [color=magenta,penwidth=" << (*iz)->support['M'] << "];\n";
        }
        if((*it)->pos != (*iz)->R->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [color=magenta,penwidth=" << (*iz)->support['M'] << "];\n";
        }
      }



      if((*iz)->support['V'] > 0){
        if((*it)->pos != (*iz)->L->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [colo\
r=green,penwidth=" << (*iz)->support['V'] << "];\n";
        }
        if((*it)->pos != (*iz)->R->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [colo\
r=green,penwidth=" << (*iz)->support['V'] << "];\n";
        }
      }      

      if((*iz)->support['L'] > 0){
	if((*it)->pos != (*iz)->L->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [color=brown,penwidth=" << (*iz)->support['L'] << "];\n";
	}
        if((*it)->pos != (*iz)->R->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [color=brown,penwidth=" << (*iz)->support['L'] << "];\n";
	}
      }
      if((*iz)->support['H'] > 0){
        if((*it)->pos != (*iz)->L->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [color=purple,penwidth=" << (*iz)->support['H'] << "];\n";
        }
        if((*it)->pos != (*iz)->R->pos){
          ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [color=purple,penwidth=" << (*iz)->support['H'] << "];\n";
	}
      }

      if((*iz)->support['S'] > 0){
	if((*it)->pos != (*iz)->L->pos){
	  ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [style=dotted,penwidth=" << (*iz)->support['S'] << "];\n";
	}
	if((*it)->pos != (*iz)->R->pos){
	  ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [style=dotted,penwidth=" << (*iz)->support['S'] << "];\n";
	}
      }
      if((*iz)->support['I'] > 0){
	if((*it)->pos != (*iz)->L->pos){
	  ss << "     " << (*it)->seqid << "." << (*it)->pos <<  " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [color=red,penwidth=" << (*iz)->support['I'] << "];\n";
	}
	if((*it)->pos != (*iz)->R->pos){
	  ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [color=red,penwidth=" << (*iz)->support['I'] << "];\n";
	}
      }
      if((*iz)->support['D'] > 0){
	if((*it)->pos != (*iz)->L->pos){
	  ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->L->seqid << "." << (*iz)->L->pos << " [color=blue,penwidth=" << (*iz)->support['D'] << "];\n";
	}
	if((*it)->pos != (*iz)->R->pos){
	  ss << "     " << (*it)->seqid << "." << (*it)->pos << " -- " << (*iz)->R->seqid << "." << (*iz)->R->pos << " [color=blue,penwidth=" << (*iz)->support['D'] << "];\n";
	}
      }
    }
  }

  ss << "}";


  return ss.str();
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : purges poorly supported trees in forest

 Function does   : dumps and shrinks graph

 Function returns: NA

*/

    void thin(){
  
  map<int, map<int, int> > lookup;
  
  map<int, map<int, int> > toDelete; 
  
  
  for(map<int, map<int, node* > >::iterator it = globalGraph.nodes.begin();it != globalGraph.nodes.end(); it++){
    for(map<int, node*>::iterator itt = it->second.begin(); itt != it->second.end(); itt++){
      
      if(lookup[it->first].find(itt->first) != lookup[it->first].end() ){
      }
      else{
	lookup[it->first][itt->first] = 1;
	
	vector<node *> tree;
	
	getTree(globalGraph.nodes[it->first][itt->first], tree);
	
	int flag = 0;
	
	for(vector<node *>::iterator ir = tree.begin(); ir != tree.end(); ir++){
	  lookup[(*ir)->seqid][(*ir)->pos] = 1;
	  for(vector<edge *>::iterator iz = (*ir)->eds.begin(); 
	      iz != (*ir)->eds.end(); iz++){
	    if((*iz)->support['I'] > 2 || (*iz)->support['D'] > 2 || (*iz)->support['S'] > 2 || (*iz)->support['L'] > 2 || (*iz)->support['R'] > 2 ){
	      flag = 1;
	    }
	  }
	}
	
	if(flag == 0){
	  for(vector<node *>::iterator ir = tree.begin(); ir != tree.end(); ir++){
	    toDelete[(*ir)->seqid][(*ir)->pos] = 1;	    
	  }
	}
      }
    }
  }
  
  for(map< int, map<int, int> >::iterator td = toDelete.begin(); td != toDelete.end(); td++){
    for(map<int, int>::iterator tdz = toDelete[td->first].begin(); tdz != toDelete[td->first].end(); tdz++){
      
      for(vector<edge *>::iterator etd = globalGraph.nodes[td->first][tdz->first]->eds.begin(); 
	  etd != globalGraph.nodes[td->first][tdz->first]->eds.end(); etd++){
	//	delete (*etd);
      }

      delete globalGraph.nodes[td->first][tdz->first];
      globalGraph.nodes[td->first].erase(tdz->first);
    }
  }
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of node pointers

 Function does   : tries to resolve a deletion

 Function returns: NA

*/

bool detectInsertion(vector<node *> tree, breakpoints * bp){

  vector <node *> putative;
  
  for(vector<node * >::iterator t = tree.begin(); t != tree.end(); t++){
    
    if((*t)->eds.size() > 1 ){
      
      int tooClose    = 0;
      int splitR      = 0;
      int insertion   = 0;

      for(vector<edge *>::iterator es = (*t)->eds.begin(); es != (*t)->eds.end(); es++){
        tooClose  += (*es)->support['H'];
	splitR    += (*es)->support['S'];
	insertion += (*es)->support['I'];
      }
      if( tooClose > 0 && insertion  > 0 && splitR == 0){
	putative.push_back((*t));
      }
    } 
  }
  if(putative.size() == 2){

    int lPos = putative.front()->pos;
    int rPos = putative.back()->pos ;

    int lhit = 0 ; int rhit = 0;

    for(vector<edge *>::iterator ed = putative.front()->eds.begin() ;
        ed != putative.front()->eds.end(); ed++){
      if(((*ed)->L->pos == rPos) || ((*ed)->R->pos == rPos)){
        lhit = 1;
        break;
      }
    }

    for(vector<edge *>::iterator ed = putative.back()->eds.begin() ;
        ed != putative.back()->eds.end(); ed++){
      if(((*ed)->L->pos == lPos) || ((*ed)->R->pos == lPos)){
	rhit = 1;
        break;
      }
    }
    
    if(lhit == 1 && rhit == 1){
      //      cerr << "insertion pair: " << putative.front()->seqid  << " " <<  lPos << "\t" << rPos << endl;
      return true;
    }
    else{
      cerr << "no linked putative breakpoints" << endl;
    }
    
  }

  return false;
}



//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : two node pointer

 Function does   : finds if nodes are directly connected

 Function returns: bool

*/

bool connectedNode(node * left, node * right){

  for(vector<edge *>::iterator l = left->eds.begin(); l != left->eds.end(); l++){
    
    if((*l)->L->pos == right->pos || (*l)->R->pos == right->pos){
      return true;
    }
  }
  return false;
}


/*
  Function input  : a vector of edge pointers 

  Function does   : does a tree terversal and joins nodes

  Function returns: NA

*/


bool findEdge(vector<edge *> & eds, edge ** e, int pos){
  
  //  cerr << "finding edge" << endl;

  for(vector<edge *>::iterator it = eds.begin(); it != eds.end(); it++){
    if((*it)->L->pos == pos || (*it)->R->pos == pos ){
      (*e) = (*it);
      // cerr << "found edge: " << (*e)->L->pos  << endl;
      return true;
    }
  }

  return false;
}



//------------------------------- SUBROUTINE --------------------------------
/*
  Function input  : a vector of node pointers, and a postion

  Function does   : removes edges that contain a position

  Function returns: NA

*/


void removeEdges(vector<node *> & tree, int pos){

  //  cerr << "removing edges" << endl;

  for(vector<node *>::iterator rm = tree.begin(); rm != tree.end(); rm++){

    vector<edge *> tmp;

    for(vector<edge *>::iterator e = (*rm)->eds.begin(); e != (*rm)->eds.end(); e++){
      if( (*e)->L->pos != pos && (*e)->R->pos != pos  ){
	tmp.push_back((*e));
      }
      else{
	// delete the edge

      }
    }
    (*rm)->eds.clear();
    (*rm)->eds.insert((*rm)->eds.end(), tmp.begin(), tmp.end());
  }
}


//------------------------------- SUBROUTINE --------------------------------
/*
  Function input  : a vector of node pointers

  Function does   : does a tree terversal and joins nodes

  Function returns: NA

*/

void joinNodes(node * L, node * R, vector<node *> & tree){

  int lSupport = 0;
  int rSupport = 0;
  
  for(vector<edge *>::iterator le = L->eds.begin(); le != L->eds.end(); le++){
    lSupport += (*le)->support['D'] + (*le)->support['S'] + (*le)->support['I']
      +  (*le)->support['H'] +  (*le)->support['L'];
  }
  for(vector<edge *>::iterator re = R->eds.begin(); re != R->eds.end(); re++){
    lSupport += (*re)->support['D'] + (*re)->support['S'] + (*re)->support['I']
      +  (*re)->support['H'] +  (*re)->support['L'];
  }
  
  //  cerr << "Joining nodes: " << L->pos << " " << R->pos << endl;

  if(lSupport <= rSupport){

    //cerr << "Joining left" << endl;

    L->collapsed = true;

    for(vector<edge *>::iterator lc =  L->eds.begin(); lc != L->eds.end(); lc++){

      //cerr << "edge: " <<  (*lc)->L->pos << " " << (*lc)->R->pos << endl;

      edge * e; 

      int otherP = (*lc)->L->pos;

      if(L->pos  == otherP){
	otherP = (*lc)->R->pos;
      }
      if(findEdge(R->eds, &e,  otherP)){

	//cerr << "P: " <<  e->L->pos << endl;

	e->support['I'] += (*lc)->support['I'];
	e->support['D'] += (*lc)->support['D'];
	e->support['S'] += (*lc)->support['S'];
	e->support['H'] += (*lc)->support['H'];
	e->support['L'] += (*lc)->support['L'];
	e->support['R'] += (*lc)->support['R'];
	e->support['M'] += (*lc)->support['M'];
	e->support['V'] += (*lc)->support['V'];
	e->support['X'] += (*lc)->support['X'];
      }
      else{
	if((*lc)->L->pos == otherP){
	  (*lc)->R = R;
	}
	else{
	  (*lc)->L = R;
	}

	R->eds.push_back(*lc);
      }

    }
    removeEdges(tree, L->pos);
 
  }
  else{

    //    cerr << "Joining right" << endl;
    R->collapsed = true;

    for(vector<edge *>::iterator lc =  R->eds.begin(); lc != R->eds.end(); lc++){

      edge * e;

      int otherP = (*lc)->L->pos;

      if(R->pos  == otherP){
	otherP = (*lc)->R->pos;
      }
      if(findEdge(L->eds, &e,  otherP)){
        e->support['I'] += (*lc)->support['I'];
        e->support['D'] += (*lc)->support['D'];
        e->support['S'] += (*lc)->support['S'];
	e->support['H'] += (*lc)->support['H'];
	e->support['L'] += (*lc)->support['L'];
	e->support['M'] += (*lc)->support['M'];
	e->support['X'] += (*lc)->support['X'];
	e->support['V'] += (*lc)->support['V'];
      }
      else{
        if((*lc)->L->pos == otherP){
          (*lc)->R = L;
	}
        else{
          (*lc)->L = L;
        }
        L->eds.push_back(*lc);
	
      }
    }
    removeEdges(tree, R->pos);
  }

}



//------------------------------- SUBROUTINE --------------------------------
/*
  Function input  : a vector of node pointers
  
  Function does   : does a tree terversal and joins nodes
  
  Function returns: NA
  
*/

void collapseTree(vector<node *> & tree){

  //  cerr << "Collapsing" << endl;

  vector<node *> tmp;

  for(vector<node *>::iterator tr = tree.begin(); tr != tree.end(); tr++){
    if((*tr)->collapsed){
//      cerr << "ci" << endl;
      continue;
    }
    for(vector<node *>::iterator tt = tree.begin(); tt != tree.end(); tt++){
      if((*tt)->collapsed){
	//      cerr << "cii" << endl;
        continue;
      }
      if( (*tr)->pos == (*tt)->pos ){
	//      cerr << "ciii" << endl;
        continue;
      }
      //      cerr << "N1: " << (*tr)->pos << " N2: " << (*tt)->pos << endl;

      if(abs( (*tr)->pos - (*tt)->pos ) < 20 && ! connectedNode((*tr), (*tt))){
	//neighborNode((*tr)->eds, (*tt)->eds);
	
	joinNodes((*tr), (*tt), tree);
	
	//	cerr << "close: " << (*tr)->pos << " " << (*tt)->pos << " " << shared << endl;
      
      }
    }
  }

  for(vector<node *>::iterator tr = tree.begin(); tr != tree.end(); tr++){
    if((*tr)->collapsed){
    }
    else{
      tmp.push_back((*tr));
    }
  }

  tree.clear();
  tree.insert(tree.end(), tmp.begin(), tmp.end());

}



//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of node pointers

 Function does   : tries to resolve an inversion

 Function returns: NA

*/

bool detectInversion(vector<node *> & tree, breakpoints * bp){

  vector <node *> putative;

  for(vector<node * >::iterator t = tree.begin(); t != tree.end(); t++){


    int tooFar  = 0;
    int splitR  = 0;
    int del     = 0;

    for(vector<edge *>::iterator es = (*t)->eds.begin(); es != (*t)->eds.end(); es++){

      tooFar += (*es)->support['M'];
      splitR += (*es)->support['V'];
      del    += (*es)->support['D'];


      //                      cerr << "counts :" << (*t)->pos << "\t" << (*es)->support['H'] << " " << (*es)->support['H']
      //                   << " " << tooFar << " " << splitR << endl;
    }

    if( (tooFar > 0 && splitR > 1) || (del > 1 && splitR > 0) || splitR > 1){
      putative.push_back((*t));
    }
  }

  if(putative.size() == 2){
    
    int lPos = putative.front()->pos;
    int rPos = putative.back()->pos ;
    
    int lhit = 0 ; int rhit = 0;
    
    int totalS = 0;

    for(vector<edge *>::iterator ed = putative.front()->eds.begin() ;
        ed != putative.front()->eds.end(); ed++){
      if(((*ed)->L->pos == rPos) || ((*ed)->R->pos == rPos)){
        lhit = 1;
	totalS += (*ed)->support['L'];
	totalS += (*ed)->support['H'];
	totalS += (*ed)->support['S'];
	totalS += (*ed)->support['I'];
	totalS += (*ed)->support['D'];
	totalS += (*ed)->support['V'];
	totalS += (*ed)->support['M'];
	totalS += (*ed)->support['R'];
	totalS += (*ed)->support['X'];
        break;
      }
    }
    
    for(vector<edge *>::iterator ed = putative.back()->eds.begin() ;
        ed != putative.back()->eds.end(); ed++){
      if(((*ed)->L->pos == lPos) || ((*ed)->R->pos == lPos)){
        rhit = 1;
        break;
      }
    }

    
    if(lPos > rPos){
      int tmp = lPos;
      lPos = rPos;
      rPos = tmp ;
    }

    if((rPos - lPos) > 1500000 ){
      if(totalS < 5){
	return  false;
      }
    }

    if(lhit == 1 && rhit == 1){
      bp->two           = true                   ;
      bp->type          = 'V'                    ;
      bp->merged        =  0                     ; 
      bp->seqidIndexL   = putative.front()->seqid;
      bp->seqidIndexR   = putative.front()->seqid;
      bp->five          = lPos                   ;
      bp->three         = rPos                   ;
      bp->svlen         = rPos - lPos            ;
      bp->totalSupport  = totalS                 ;
      return true;
    }
    else{
      cerr << "no linked putative breakpoints" << endl;
    }
  }
  return false;
}




//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of node pointers

 Function does   : tries to resolve a duplication

 Function returns: NA

*/

bool detectDuplication(vector<node *> tree, breakpoints * bp){

  vector <node *> putative;

  for(vector<node * >::iterator t = tree.begin(); t != tree.end(); t++){

    int tooFar   = 0;
    int tooClose = 0;
    int FlippedS = 0;

    for(vector<edge *>::iterator es = (*t)->eds.begin(); es != (*t)->eds.end(); es++){

      tooFar   += (*es)->support['H'];
      tooClose += (*es)->support['L'];
      FlippedS += (*es)->support['X'];


    }
    if( (tooFar > 0 && FlippedS  > 1) || FlippedS  > 1 || (tooClose  > 0 && FlippedS  > 1) ){
      putative.push_back((*t));
    }
  }
  if(putative.size() == 2){

    int lPos = putative.front()->pos;
    int rPos = putative.back()->pos ;

    int lhit = 0 ; int rhit = 0;

    int totalS = 0;

    for(vector<edge *>::iterator ed = putative.front()->eds.begin() ;
        ed != putative.front()->eds.end(); ed++){
      if(((*ed)->L->pos == rPos) || ((*ed)->R->pos == rPos)){
        lhit = 1;
	totalS += (*ed)->support['L'];
        totalS += (*ed)->support['H'];
        totalS += (*ed)->support['S'];
        totalS += (*ed)->support['I'];
        totalS += (*ed)->support['D'];
        totalS += (*ed)->support['V'];
        totalS += (*ed)->support['M'];
        totalS += (*ed)->support['R'];
        totalS += (*ed)->support['X'];

        break;
      }
    }

    for(vector<edge *>::iterator ed = putative.back()->eds.begin() ;
        ed != putative.back()->eds.end(); ed++){
      if(((*ed)->L->pos == lPos) || ((*ed)->R->pos == lPos)){
        rhit = 1;
        break;
      }
    }
    
    if(lPos > rPos){
      int tmp = lPos;
      lPos    = rPos;
      rPos    = tmp ;
    }
    if((rPos - lPos) > 1500000 ){
      if(totalS < 5){
	return  false;
      }
    }

    if(lhit == 1 && rhit == 1){
      bp->two         = true                   ;
      bp->type        = 'U'                    ;
      bp->merged      = 0                      ;
      bp->seqidIndexL = putative.front()->seqid;
      bp->seqidIndexR = putative.front()->seqid;
      bp->five        = lPos                   ;
      bp->three       = rPos                   ;
      bp->svlen       = rPos - lPos            ;
      bp->totalSupport  = totalS               ;
      return true;
    }
    else{
      cerr << "no linked putative breakpoints" << endl;
    }
    
  }
  else if(putative.size() > 2){

    vector <node *> putativeTwo;
    cerr << "greater than two breakpoints: " << putative.size() << " "  << putative.front()->pos  << endl;
  }
  else{
    // leaf node
  }
  
  return false;
}



//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of node pointers

 Function does   : tries to resolve a deletion with no links

 Function returns: NA

*/

bool detectHalfDeletion(vector<node *> tree, breakpoints * bp, node ** n){
  
  vector <node *> putative;

  for(vector<node * >::iterator t = tree.begin(); t != tree.end(); t++){

    int tooFar  = 0;
    int splitR  = 0;
    int del     = 0;

    for(vector<edge *>::iterator es = (*t)->eds.begin(); es != (*t)->eds.end(); es++){
      tooFar += (*es)->support['H'];
      splitR += (*es)->support['S'];
      del    += (*es)->support['D'];
    }
    if( tooFar > 2){
      putative.push_back((*t));
    }
  }
  if(putative.size() == 1){
    bp->seqidIndexL = putative.front()->seqid;
    bp->five        = putative.front()->pos  ;
    *n = putative.front();

    return true;
  }

  return false;
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : vector of node pointers

 Function does   : tries to resolve a deletion

 Function returns: NA

*/


bool detectDeletion(vector<node *> tree, breakpoints * bp){
  
  vector <node *> putative;

  for(vector<node * >::iterator t = tree.begin(); t != tree.end(); t++){
   
      int tooFar  = 0; 
      int splitR  = 0;
      int del     = 0;

      for(vector<edge *>::iterator es = (*t)->eds.begin(); es != (*t)->eds.end(); es++){

	tooFar += (*es)->support['H'];
	splitR += (*es)->support['S'];
	del    += (*es)->support['D'];

      }
      if( (tooFar > 0 && splitR > 1) || (del > 1 && splitR > 0) || splitR > 1){
	putative.push_back((*t));
      } 
  }
  
  if(putative.size() == 2){
    
    int lPos = putative.front()->pos;
    int rPos = putative.back()->pos ;
    
    int lhit = 0 ; int rhit = 0;
    
    int totalS = 0;

    for(vector<edge *>::iterator ed = putative.front()->eds.begin() ;
	ed != putative.front()->eds.end(); ed++){
      if(((*ed)->L->pos == rPos) || ((*ed)->R->pos == rPos)){
	lhit = 1; 
	totalS += (*ed)->support['L'];
        totalS += (*ed)->support['H'];
        totalS += (*ed)->support['S'];
        totalS += (*ed)->support['I'];
        totalS += (*ed)->support['D'];
        totalS += (*ed)->support['V'];
        totalS += (*ed)->support['M'];
        totalS += (*ed)->support['R'];
        totalS += (*ed)->support['X'];


	break;
      }
    }
    
    for(vector<edge *>::iterator ed = putative.back()->eds.begin() ;
        ed != putative.back()->eds.end(); ed++){
      if(((*ed)->L->pos == lPos) || ((*ed)->R->pos == lPos)){
        rhit = 1;
	break;
      }
    }  
    if(lPos > rPos){
      int tmp = lPos;
      lPos    = rPos;
      rPos    = tmp ;
    }

    if((rPos - lPos) > 1500000 ){
      if(totalS < 5){
	return  false;
      }
    }


    if(lhit == 1 && rhit == 1){
      bp->two         = true                   ;
      bp->type        = 'D'                    ;
      bp->seqidIndexL = putative.front()->seqid;
      bp->seqidIndexR = putative.front()->seqid;
      bp->merged      = 0                      ;
      bp->five        = lPos                   ;
      bp->three       = rPos                   ;
      bp->svlen       = rPos - lPos            ;
      bp->totalSupport = totalS                ;
      return true;
    }
    else{
      cerr << "no linked putative breakpoints" << endl;
    }

  }
  else if(putative.size() > 2){

    vector <node *> putativeTwo;
       cerr << "greater than two breakpoints: " << putative.size() << " "  << putative.front()->pos  << endl;
  }
  else{
    // leaf node
  }
  
  return false;
}

void callBreaks(vector<node *> & tree, 
		vector<breakpoints *> & allBreakpoints, 
		map < int , map <int, node *> > & hb){

  collapseTree(tree);

  breakpoints * bp;

  node * nr; 


  bp = new breakpoints;
  bp->two = false;

  if(detectDeletion(tree, bp)){
    omp_set_lock(&lock);
    allBreakpoints.push_back(bp);
    omp_unset_lock(&lock);
  }
  else if(detectDuplication(tree, bp)){
    omp_set_lock(&lock);
    allBreakpoints.push_back(bp);
    omp_unset_lock(&lock);
  }
  else if(detectInversion(tree, bp)){
    omp_set_lock(&lock);
    allBreakpoints.push_back(bp);
    omp_unset_lock(&lock);
  }
  else if(detectHalfDeletion(tree, bp, &nr)){
    omp_set_lock(&lock);
    hb[bp->seqidIndexL][bp->five] = nr;
    delete bp;
    omp_unset_lock(&lock);
  }
  else{
    delete bp;

  }
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : processes trees

 Function does   : tries to define type and breakpoints

 Function returns: NA

*/

void gatherTrees(vector<vector<node *> > & globalTrees){
  
  map<int, map<int, int> > lookup;
  
  for(map<int, map<int, node* > >::iterator it = globalGraph.nodes.begin();it != globalGraph.nodes.end(); it++){
    for(map<int, node*>::iterator itt = it->second.begin(); itt != it->second.end(); itt++){

      if(lookup[it->first].find(itt->first) != lookup[it->first].end() ){
	//      cerr << "seen: " << it->first << " " << itt->first << endl;
      }
      else{
	lookup[it->first][itt->first] = 1;

	vector<node *> tree;
	getTree(globalGraph.nodes[it->first][itt->first], tree);
	for(vector<node *>::iterator ir = tree.begin(); ir != tree.end(); ir++){
          lookup[(*ir)->seqid][(*ir)->pos] = 1;
        }

	globalTrees.push_back(tree);


      }
    }
  }
}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : nothing

 Function does   : dumps and shrinks graph

 Function returns: NA

*/


void dump(vector< vector< node *> > & allTrees){
  
  ofstream graphOutFile;

  graphOutFile.open(globalOpts.graphOut);

  for(vector< vector<node *> >::iterator it = allTrees.begin(); 
      it != allTrees.end(); it++){
    graphOutFile << dotviz(*it) << endl << endl;
  }

  graphOutFile.close();
}


//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : bam file name and breakpoint

 Function does   : aligns reads to breakpoints

 Function returns: NA
*/

void genotype(string & bamF, breakpoints * br){

  BamReader bamR;
  if(!bamR.Open(bamF)){
    cerr << "FATAL: count not open bamfile: " << bamF;
  }
  if(! bamR.LocateIndex()){
    vector<string> fileName = split(bamF, ".");
    fileName.back() = "bai";
    string indexName = join(fileName, ".");
    if(! bamR.OpenIndex(indexName) ){
      cerr << "FATAL: cannot find bam index." << endl;
    }
  }

  if(!bamR.SetRegion(br->seqidIndexL, br->five - 1, br->seqidIndexL, br->five + 1)){
    cerr << "FATAL: cannot set region for genotyping." << endl;
  }

  vector<BamAlignment> reads;

  BamAlignment al;

  int nr = 0;

  while(bamR.GetNextAlignment(al)){
    if((al.AlignmentFlag & 0x0800) != 0 ){
      continue;
    }
    if(! al.IsPaired()){
      continue;
    }
    if(! al.IsMapped() && ! al.IsMateMapped()){
      continue;
    }
    if(al.IsDuplicate()){
      continue;
    }
    if(! al.IsPrimaryAlignment()){
      continue;
    }
    nr += 1;
    if(nr > 100){
      break;
    }
    reads.push_back(al);
  }
  if(br->two){
    if(!bamR.SetRegion(br->seqidIndexL, br->three -1, br->seqidIndexL, br->three +1)){
      cerr << "FATAL: cannot set region for genotyping." << endl;
      exit(1);
    }

    nr = 0;

    while(bamR.GetNextAlignment(al)){
      if((al.AlignmentFlag & 0x0800) != 0 ){
	continue;
      }
      if(! al.IsPaired()){
	continue;
      }
      if(! al.IsMapped() && ! al.IsMateMapped()){
	continue;
      }
      if(al.IsDuplicate()){
	continue;
      }
      if(! al.IsPrimaryAlignment()){
	continue;
      }
      if(nr > 100){
	break;
      }
      reads.push_back(al);
    }
  }

  // Declares a default Aligner
  StripedSmithWaterman::Aligner aligner;
  // Declares a default filter
  StripedSmithWaterman::Filter filter;
  // Declares an alignment that stores the result
  StripedSmithWaterman::Alignment alignment;
  // Aligns the query to the ref

  vector<double> refScores ;
  vector<double> altScores ;
  vector<double> alt2Scores;

  double aal = 0;
  double abl = 0;
  double bbl = 0;

  for(vector<BamAlignment>::iterator it = reads.begin(); it != reads.end(); it++){

    aligner.Align((*it).QueryBases.c_str(), br->alleles.front().c_str(), 
		  br->alleles.front().size(), filter, &alignment);

    refScores.push_back(double(alignment.sw_score));
    aligner.Align((*it).QueryBases.c_str(), br->alleles.back().c_str(),  
		  br->alleles.back().size(),  filter, &alignment);

    altScores.push_back(double(alignment.sw_score));
    
    double mappingP = -1;

    if( altScores.back() >= refScores.back() ){
      
      mappingP = 1 - (altScores.back() / (altScores.back() + refScores.back())) ;

      if(mappingP == 1 ){
	mappingP = 0.9999;
      }
      if(mappingP == 0 ){
	mappingP = 0.0001;
      }

      // alt
      aal += log((2-2) * (1-mappingP) + (2*mappingP)) ;
      abl += log((2-1) * (1-mappingP) + (1*mappingP)) ;
      bbl += log((2-0) * (1-mappingP) + (0*mappingP)) ;
    }
    else{
      mappingP = 1 - (refScores.back() / (altScores.back() + refScores.back()));

      if(mappingP == 1 ){
        mappingP = 0.9999;
      }
      if(mappingP == 0 ){
        mappingP = 0.0001;
      }
      //ref
      aal += log((2 - 2)*mappingP + (2*(1-mappingP)));
      abl += log((2 - 1)*mappingP + (1*(1-mappingP)));
      bbl += log((2 - 0)*mappingP + (0*(1-mappingP)));
    }
  }

  aal = aal - log(pow(2,reads.size())); 
  abl = abl - log(pow(2,reads.size())); 
  bbl = bbl - log(pow(2,reads.size()));

  vector<double> gl;
  gl.push_back(aal);
  gl.push_back(abl);
  gl.push_back(bbl);
 
  int index = 0;
  if(abl > aal && abl > bbl){
    index = 1;
  }
  if(bbl > aal && bbl > abl){
    index = 2;
  }


  double aal2 = 0;
  double abl2 = 0;
  double bbl2 = 0;

  if(br->alleles.size() > 2){

    for(vector<BamAlignment>::iterator it = reads.begin(); it != reads.end(); it++){

      aligner.Align((*it).QueryBases.c_str(), br->alleles.front().c_str(),
		    br->alleles.front().size(), filter, &alignment);

      aligner.Align((*it).QueryBases.c_str(), br->alleles[1].c_str(),
		    br->alleles[1].size(),  filter, &alignment);

      alt2Scores.push_back(double(alignment.sw_score));

      double mappingP = -1;

      if( altScores.back() >= refScores.back() ){
	
	mappingP = 1 - (altScores.back() / (altScores.back() + refScores.back())) ;
	
	if(mappingP == 1 ){
	  mappingP = 0.9999;
	}
	if(mappingP == 0 ){
	  mappingP = 0.0001;
	}
	// alt
	aal += log((2-2) * (1-mappingP) + (2*mappingP)) ;
	abl += log((2-1) * (1-mappingP) + (1*mappingP)) ;
	bbl += log((2-0) * (1-mappingP) + (0*mappingP)) ;
      }
      else{
	mappingP = 1 - (refScores.back() / (altScores.back() + refScores.back()));
	
	if(mappingP == 1 ){
	  mappingP = 0.9999;
	}
	if(mappingP == 0 ){
	  mappingP = 0.0001;
	}
	//ref
	aal += log((2 - 2)*mappingP + (2*(1-mappingP)));
	abl += log((2 - 1)*mappingP + (1*(1-mappingP)));
	bbl += log((2 - 0)*mappingP + (0*(1-mappingP)));
      }
    }

    aal2 = aal2 - log(pow(2,reads.size()));
    abl2 = abl2 - log(pow(2,reads.size()));
    bbl2 = bbl2 - log(pow(2,reads.size()));

  }

  vector<double> gl2;
  gl2.push_back(aal2);
  gl2.push_back(abl2);
  gl2.push_back(bbl2);

//  int index2 = 0;
//  if(abl2 > aal2 && abl2 > bbl2){
//    index2 = 1;
//  }
//  if(bbl2 > aal2 && bbl2 > abl2){
//    index2 = 2;
//  }
//  
//  if(gl[index] < gl2[index2]){
//    gl.clear();
//    gl.push_back(aal2);
//    gl.push_back(abl2);
//    gl.push_back(bbl2);
//    index = index2;
//    br->type = 'U';
//  }


  br->genotypeLikelhoods.push_back(gl);
  br->genotypeIndex.push_back(index);

  bamR.Close();

}

//------------------------------- SUBROUTINE --------------------------------
/*
 Function input  : bam file

 Function does   : dumps and shrinks graph

 Function returns: NA
*/
  
void gatherBamStats(string & targetfile){

  omp_set_lock(&lock);
  int quals [126];
  memcpy(quals, SangerLookup, 126*sizeof(int));

  FastaReference RefSeq;
  RefSeq.open(globalOpts.fasta);

  cerr << "INFO: gathering stats (may take some time) for bam: " << targetfile << endl;
  
  omp_unset_lock(&lock);

  vector<double> alIns        ;
  vector<double> nReads       ;
  vector<double> randomSWScore;

  BamReader bamR;
  if(!bamR.Open(targetfile)   ){
  cerr << "FATAL: cannot find - or - read : " << targetfile << endl;
    exit(1);
  }

  if(! bamR.LocateIndex()){
  cerr << "FATAL: cannot find - or - open index for : " << targetfile << endl;
    exit(1);
  }

  SamHeader SH = bamR.GetHeader();
  if(!SH.HasSortOrder()){
  cerr << "FATAL: sorted bams must have the @HD SO: tag in each SAM header." << endl;
    exit(1);
  }

  // Declares a default Aligner
  StripedSmithWaterman::Aligner aligner;
  // Declares a default filter
  StripedSmithWaterman::Filter filter;
  // Declares an alignment that stores the result
  StripedSmithWaterman::Alignment alignment;
  // Aligns the query to the ref

  RefVector sequences = bamR.GetReferenceData();

  int i = 0; // index for while loop
  int n = 0; // number of reads

  BamAlignment al;

  int qsum = 0;
  int qnum = 0;

  int fail = 0;

  while(i < 8 || n < 100000){

    if((n % 100) == 0){
      omp_set_lock(&lock);
      cerr << "INFO: processed " << n << " reads for: " << targetfile << endl;
      omp_unset_lock(&lock);
    }

    fail += 1;
    if(fail > 1000000){
      cerr << "FATAL: was not able to gather stats on bamfile: " << targetfile << endl;
      exit(1);
    }

    unsigned int max = 20;

    if(sequences.size() < max){
      max = sequences.size() ;
    }

    int randomChr = rand() % (max -1);
    int randomPos = rand() % (sequences[randomChr].RefLength -1);
    int randomEnd = randomPos + 2000;

    if(randomEnd > sequences[randomChr].RefLength){
      continue;
    }

    if(! bamR.SetRegion(randomChr, randomPos, randomChr, randomEnd)){
      cerr << "FATAL: Cannot set random region";
      exit(1);
    }

    if(!bamR.GetNextAlignmentCore(al)){
      continue;
    }

    i++;

    long int cp = al.GetEndPosition(false,true);

    readPileUp allPileUp;
    while(bamR.GetNextAlignment(al)){
      if(!al.IsMapped() || ! al.IsProperPair()){
        continue;
      }
      string any;
      if(al.GetTag("XA", any)){
        continue;
      }
      if(al.GetTag("SA", any)){
        continue;
      }
      if(al.IsDuplicate()){
	continue;
      }
      
//      int randomChr2 = rand() % (max -1);
//      int randomPos2 = rand() % (sequences[randomChr].RefLength -1);
//      int randomEnd2 = randomPos + 10000;

//      int randomL = ( (rand() % 200) - 200 ) + al.Position;

//      while(randomEnd2 > sequences[randomChr].RefLength){
//	randomChr2 = rand() % (max -1);
//	randomPos2 = rand() % (sequences[randomChr].RefLength -1);
//	randomEnd2 = randomPos + 10000;
//      }

//      string RefChunk = RefSeq.getSubSequence(sequences[randomChr].RefName, randomL, 200);
					      
//      aligner.Align(al.QueryBases.c_str(), RefChunk.c_str(), RefChunk.size(), filter, &alignment);      

//      if(alignment.sw_score > 0){
//	randomSWScore.push_back(double(alignment.sw_score));
//      }      

      string squals = al.Qualities;
      
      // summing base qualities (quals is the lookup)
      
      for(unsigned int q = 0 ; q < squals.size(); q++){
        qsum += quals[ int(squals[q]) ];
        qnum += 1;
	
        if(quals[int(squals[q])] < 0){
          omp_set_lock(&lock);
          cerr << endl;
          cerr << "FATAL: base quality is not sanger or illumina 1.8+ (0,41) in file : " << targetfile << endl;
          cerr << "INFO : offending qual string   : " << squals << endl;
          cerr << "INFO : offending qual char     : " << squals[q] << endl;
          cerr << "INFO : -1 qual ; qual ; qual +1: " << quals[ int(squals[q]) -1 ] << " " << quals[ int(squals[q])  ] << " " << quals[ int(squals[q]) +1 ] << endl;
          cerr << "INFO : rescale qualities or contact author for additional quality ranges" << endl;
          cerr << endl;
          omp_unset_lock(&lock);
          exit(1);
        }
      }

      if(al.Position > cp){
        allPileUp.purgePast(&cp);
        cp = al.GetEndPosition(false,true);
        nReads.push_back(allPileUp.currentData.size());
      }
      if(al.IsMapped()
         && al.IsMateMapped()
         && abs(double(al.InsertSize)) < 10000
         && al.RefID == al.MateRefID
         ){
        allPileUp.processAlignment(al);
        alIns.push_back(abs(double(al.InsertSize)));
        n++;
      }
    }
  }
  bamR.Close();
  
  sort(alIns.begin(), alIns.end()     );

  int index = 0;

  if((alIns.size() % 2) != 0 ){
    index = ((alIns.size()+1)/2)-1;
  }
  else{
    index = (alIns.size()/2)-1;
  }

  double median   = alIns[index];
  double mu       = mean(alIns        );
  double mud      = mean(nReads       );
  double variance = var(alIns, mu     );
  double sd       = sqrt(variance     );
  double sdd      = sqrt(var(nReads, mud ));
  
  //  double sw_mu    = mean(randomSWScore);
  //  double sw_sd    = sqrt(var(randomSWScore, sw_mu));
 
 omp_set_lock(&lock);

 insertDists.mus[  targetfile ] = mu;
 insertDists.sds[  targetfile ] = sd;
 insertDists.avgD[ targetfile ] = mud;
 // insertDists.swm[ targetfile ] = sw_mu;
 // insertDists.sws[ targetfile ] = sw_sd;
 

 // cerr << "dist: " << join(randomSWScore, ",") << endl;

 cerr << "INFO: for file:" << targetfile << endl
      << "      " << targetfile << ": mean depth: ......... " << mud << endl
      << "      " << targetfile << ": sd depth: ........... " << sdd << endl
   //      << "      " << targetfile << ": mean SW alignments .. " << sw_mu << endl
   //      << "      " << targetfile << ": sd SW alignments .... " << sw_sd << endl
      << "      " << targetfile << ": mean insert length: . " << insertDists.mus[targetfile] << endl
      << "      " << targetfile << ": median insert length. " << median                      << endl
      << "      " << targetfile << ": sd insert length .... " << insertDists.sds[targetfile] << endl
      << "      " << targetfile << ": lower insert length . " << insertDists.mus[targetfile] - (2.5*insertDists.sds[targetfile])   << endl
      << "      " << targetfile << ": upper insert length . " << insertDists.mus[targetfile] + (2.5*insertDists.sds[targetfile])   << endl
      << "      " << targetfile << ": average base quality: " << double(qsum)/double(qnum) << " " << qsum << " " << qnum << endl
      << "      " << targetfile << ": number of reads used: " << n  << endl << endl;

  omp_unset_lock(&lock);
}


int avgP(node * n){
  
  double pi  = 0;
  double ni  = 0;

  for(vector<edge *>::iterator it = n->eds.begin(); it != n->eds.end(); it++){
    
    if((*it)->L->pos == n->pos){  
      pi += double((*it)->R->pos) * double((*it)->support['H']);
      ni += double((*it)->support['H'])                        ;  
    }
    else{
      pi += double((*it)->L->pos) * double((*it)->support['H']);
      ni += double((*it)->support['H'])                        ;  
    }
    
  }

  return int(double(pi) / double(ni));

}

void mergeDels(map <int, map <int, node * > > & hf, vector< breakpoints *> & br){

  map<int, map<int, int> > seen;

  for(map <int, map <int, node * > >::iterator hfs = hf.begin(); 
      hfs != hf.end(); hfs++){

    for(map<int, node*>::iterator hpos = hfs->second.begin(); 
	hpos != hfs->second.end(); hpos++){
    
      if(seen[hpos->second->seqid].find(hpos->second->pos) != seen[hpos->second->seqid].end()){
	continue;
      }

      int otherPos = avgP(hpos->second);
    
      for(map<int, node*>::iterator spos = hfs->second.begin(); spos != hfs->second.end(); spos++){

	if(hpos->second->pos == spos->second->pos ){
	  continue;
	}
	
	if(seen[spos->second->seqid].find(spos->second->pos) != seen[spos->second->seqid].end()){
	  continue;
	}

	int otherPosSecond = avgP(spos->second);
	
	cerr << "INFO p: " << hpos->second->pos <<  " " << otherPos        << endl;
	cerr << "INFO p: " << spos->second->pos <<  " " << otherPosSecond  << endl;
	cerr << " "  << endl;
	
	if(abs(otherPos - spos->second->pos) < 500 && abs(hpos->second->pos - otherPosSecond) < 500 ){

	  seen[spos->second->seqid][spos->second->pos] = 1; 
	  seen[spos->second->seqid][hpos->second->pos] = 1; 


	  int lPos = hpos->second->pos;
	  int rPos = spos->second->pos;
	  
	  if(lPos > rPos){
	    int tmp = lPos;
	    lPos = rPos;
	    rPos = tmp;
	  }


	  cerr << "INFO: joining deletion breakpoints: " <<  lPos << " " << rPos << endl;
	  
	  breakpoints * bp = new breakpoints;

	  bp->two          = true                   ;
	  bp->type         = 'D'                    ;
	  bp->seqidIndexL  = hpos->second->seqid    ;
	  bp->seqidIndexR  = hpos->second->seqid    ;
	  bp->merged       = 1                      ;
	  bp->five         = lPos                   ;
	  bp->three        = rPos                   ;
	  bp->svlen        = rPos - lPos            ;
	  bp->totalSupport = 0                      ;

	  br.push_back(bp);
	}		
      }
    }

    cerr << "INFO n half:" << hfs->second.size() << endl;
    
  }

}


//-------------------------------    MAIN     --------------------------------
/*
 Comments:
*/

int main( int argc, char** argv)
{
  globalOpts.nthreads = -1;
  globalOpts.statsOnly = false;
  globalOpts.skipGeno  = false;

  int parse = parseOpts(argc, argv);
  if(parse != 1){
    cerr << "FATAL: unable to parse command line correctly. Double check commands." << endl;
    cerr << endl;
    printHelp();
    exit(1);
  }


  if(globalOpts.nthreads == -1){
  }
  else{
    omp_set_num_threads(globalOpts.nthreads);
  }

  FastaReference RefSeq;
  if(globalOpts.fasta.empty()){
    cerr << "FATAL: no reference fasta provided" << endl << endl;
    printHelp();
    exit(1);
  }

  RefSeq.open(globalOpts.fasta);

  // gather the insert length and other stats

#pragma omp parallel for schedule(dynamic, 3)
  for(unsigned int i = 0; i < globalOpts.targetBams.size(); i++){     
    gatherBamStats(globalOpts.targetBams[i]);    
  }

  if(globalOpts.statsOnly){
    cerr << "INFO: Exiting as -s flag is set." << endl;
    cerr << "INFO: WHAM finished normally, goodbye! " << endl;
    return 0;
  }

 RefVector sequences;

 BamMultiReader mr;
 if(! mr.Open(globalOpts.targetBams)){
   cerr << "FATAL: issue opening all bams to extract header" << endl;
   exit(1);
 }
 else{
   sequences = mr.GetReferenceData();
   mr.Close();
 }

 // load bam has openMP inside for running regions quickly

 cerr << "INFO: Loading discordant reads into graph." << endl;

 for(vector<string>::iterator bam = globalOpts.targetBams.begin();
     bam != globalOpts.targetBams.end(); bam++){
   
   cerr << "INFO: Reading: " << *bam << endl;

   loadBam(*bam);
   
   for(map<int, map<int, node * > >::iterator seqid = globalGraph.nodes.begin();
       seqid != globalGraph.nodes.end(); seqid++){
	 
     cerr << "INFO: Number of putative breakpoints for: " << sequences[seqid->first].RefName << ": " 
	  << globalGraph.nodes[seqid->first].size() << endl;
   }
 }

 cerr << "INFO: Finished loading reads." << endl;
 
 vector<breakpoints*> allBreakpoints ;
 vector<vector<node*> > globalTrees  ;
 
 cerr << "INFO: Finding trees within forest." << endl;

 gatherTrees(globalTrees);
 
#ifdef DEBUG
 dump(globalTrees);
#endif

 map<int, map <int, node*> > delBreak;

 cerr << "INFO: Finding breakpoints in trees." << endl; 
#pragma omp parallel for schedule(dynamic, 3)
 for(unsigned int i = 0 ; i < globalTrees.size(); i++){
   
   if((i % 1000) == 0){
     omp_set_lock(&glock);     
     cerr << "INFO: Processed " << i << "/" << globalTrees.size() << " trees" << endl;
     omp_unset_lock(&glock);
   }

   if(globalTrees[i].size() > 200){
     omp_set_lock(&glock);
     cerr << "WARNING: Skipping tree, too many putative breaks." << endl;
     omp_unset_lock(&glock);
     continue;
   }
   callBreaks(globalTrees[i], allBreakpoints, delBreak);   
 }

 mergeDels(delBreak, allBreakpoints);

 cerr << "INFO: Sorting "  << allBreakpoints.size() << " putative SVs." << endl;

 sort(allBreakpoints.begin(), allBreakpoints.end(), sortBreak);
 
 cerr << "INFO: Gathering alleles." << endl;
 
 for(unsigned int z = 0; z < allBreakpoints.size(); z++){
   genAlleles(allBreakpoints[z], RefSeq, sequences);
   
   if((z % 100) == 0){
     cerr << "INFO: generated " << z << " alleles" << endl;
   }
}

 int NGeno = 0;

 if(globalOpts.skipGeno){
   printBEDPE(allBreakpoints, sequences);
   cerr << "INFO: Skipping genotyping: -k " << endl;
   cerr << "INFO: WHAM finished normally, goodbye! " << endl;
   return 0;
 }

 cerr << "INFO: Genotyping SVs." << endl;
#pragma omp parallel for
 for(unsigned int z = 0; z < allBreakpoints.size(); z++){

   //#pragma omp parallel for schedule(dynamic, 3)   
   for(unsigned int i = 0 ; i < globalOpts.targetBams.size(); i++){
     genotype(globalOpts.targetBams[i], allBreakpoints[z]        );
   }
   omp_set_lock(&glock);
   NGeno += 1;
   if((NGeno % 100) == 0 && z != 0){
     cerr << "Genotyped: " << NGeno  << "/" << allBreakpoints.size() << " SVs." << endl;
   }
   omp_unset_lock(&glock);
 }

 printBEDPE(allBreakpoints, sequences);
  
 if(!globalOpts.graphOut.empty()){
   dump(globalTrees);
 }

 cerr << "INFO: WHAM finished normally, goodbye! " << endl;
 return 0;
}
