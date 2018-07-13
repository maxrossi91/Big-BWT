/* **************************************************************************
 * pfthreads.hpp
 * 
 **************************************************************************** */
extern "C" {
#include "xerrors.h"
}
#include <fcntl.h>

#define Buf_size 40
#define Min_bwt_range 100000
#define Sa_block      100000

//static size_t get_bwt_size(char *name);
static int get_bwt_fd(char *name);
static void pc_init(sem_t *free_slots, sem_t *data_items, pthread_mutex_t *m);
static void pc_destroy(sem_t *free_slots, sem_t *data_items, pthread_mutex_t *m);
void sa2da(uint_t sa[], int_t lcp[], uint8_t d[], long dsize, long dwords, int w, int numt);

// ----- parallel conversion of sa/lcp ->da/sufLen --------------------
typedef struct {
  long start;
  long end;
} sarange;

typedef struct{
    sarange buffer[Buf_size];      // buffer prod/consumer
    int cindex;                    // consumer index in buffer
    pthread_mutex_t cons_m;        // mutex for c_index
    sem_t free_slots, data_items;  // prod/consumer semaphores
    uint_t *sa, *eos;
    int_t *lcp, *wlen;
    long dwords;
    long full_words;                // full words found, shared: use mutex to access it
} sa2da_data;

  
// -------------------------------------------------------------------------
// multithread conversion of SA and LCP array to DA and SuffixLen + extra bit  
void *sa2da_body(void *v)
{
  sa2da_data *d = (sa2da_data *) v;
  uint32_t seqid; 
  long words=0;
  while(true) {
    // --- get starting position from buffer 
    xsem_wait(&d->data_items,__LINE__,__FILE__);
    xpthread_mutex_lock(&d->cons_m,__LINE__,__FILE__);
    sarange r = d->buffer[d->cindex++ % Buf_size]; 
    xpthread_mutex_unlock(&d->cons_m,__LINE__,__FILE__);
    xsem_post(&d->free_slots,__LINE__,__FILE__);
    // exit if start is illegal
    if(r.start<0) break;
    // process range [start,end]
    for(long i=r.start;i<r.end;i++) {// see sa2da()
      int_t suffixLen = getlen(d->sa[i],d->eos,d->dwords,&seqid);
      assert(seqid<=0x7FFFFFFF);     // seqid uses at most 31 bits
      assert(suffixLen>=d->lcp[i]);     // suffix length cannot be shorter than lcp
      assert(suffixLen<=d->wlen[seqid]);// suffix length cannot be larger than word length
      if(suffixLen==d->wlen[seqid]) {   // test if full word
        words++;                     
        assert(d->lcp[i]<suffixLen);    // full words are not prefix of other suffixes
      }
      if(d->lcp[i]==suffixLen) {         // save seqid + possibily extra bit 
        d->sa[i] = seqid | (1u << 31);   // mark last bit if lcp==suffix_len;
      }
      else 
        d->sa[i] = seqid;               // save only seqid = da[i]
      d->lcp[i] = suffixLen;            // save suffix length overwriting lcp
    }    
  }
  // update total number of full_words
  xpthread_mutex_lock(&d->cons_m,__LINE__,__FILE__);
  d->full_words += words;
  xpthread_mutex_unlock(&d->cons_m,__LINE__,__FILE__);
  return NULL;
}

// transform sa[],lcp[] -> da[], suflen[] + 
// extra bit telling whether suflen[i]==lcp[i]
void sa2da(uint_t sa[], int_t lcp[], uint8_t d[], long dsize, long dwords, int w, int numt)
{
  (void) d;   // d[] only used in assertions;
  long words=0;
  if(dwords>0x7FFFFFFF) {
    cerr << "Too many words in the dictionary. Current limit: 2^31-1\n";
    exit(1);
  }
  cout << "Converting SA and LCP Array to DA and SufLen using " << numt << " threads, range " << Sa_block << endl;
  
  time_t  start = time(NULL);  
  // create eos[] array with ending position in d[] of each word
  uint_t *eos = sa + 1;
  int_t *wlen = lcp + 1;
  // save length of word i in lcp[i+1]==suflen[i] (sa[i+1]=eos[i] is the position of its eos)
  wlen[0] = eos[0];
  for(long i=1;i<dwords;i++) {
    wlen[i] = eos[i]-eos[i-1] -1;
    assert(wlen[i]>0);
    assert(d[eos[i-1]]==EndOfWord);
    assert(d[eos[i-1]+wlen[i]+1]==EndOfWord);
  }
  // convert sa,lcp -> da,suflen
  if(numt==0) { // possibly used when called from bwt_mixed
    uint32_t seqid; 
    for(long i=dwords+w+1; i<dsize; i++) {     // we are considering d[sa[i]....]     
      int_t suffixLen = getlen(sa[i],eos,dwords,&seqid);
      assert(seqid<=0x7FFFFFFF);     // seqid uses at most 31 bits
      assert(suffixLen>=lcp[i]);     // suffix length cannot be shorter than lcp
      assert(suffixLen<=wlen[seqid]);// suffix length cannot be larger than word length
      if(suffixLen==wlen[seqid]) {   // test if full word
        words++;                     
        assert(lcp[i]<suffixLen);    // full words are not prefix of other suffixes
      }
      if(lcp[i]==suffixLen) {         // save seqid + possibily extra bit 
        sa[i] = seqid | (1u << 31);  // mark last bit if lcp==suffix_len;
      }
      else 
        sa[i] = seqid;               // save only seqid = da[i]
      lcp[i] = suffixLen;            // save suffix length overwriting lcp
    }
  }
  else { // multithread code 
    pthread_t t[numt];
    sa2da_data d;
    pc_init(&d.free_slots,&d.data_items, &d.cons_m);
    d.cindex = 0; d.full_words=0;
    d.sa = sa; d.eos = eos;
    d.lcp = lcp; d.wlen = wlen; d.dwords=dwords;
    // thread creation
    for(int i=0;i<numt;i++) 
      xpthread_create(&t[i],NULL,sa2da_body,&d,__LINE__,__FILE__);
    // producer code
    sarange r; int pindex = 0;
    for(long i=dwords+w+1; i<dsize; ) {
      r.start = i;
      r.end = i+Sa_block;
      if(r.end>dsize) r.end = dsize;
      // write to the buffer
      xsem_wait(&d.free_slots,__LINE__,__FILE__);
      d.buffer[(pindex++) % Buf_size] = r; 
      xsem_post(&d.data_items,__LINE__,__FILE__);
      i = r.end;
    }
    // send terminate data
    r.start = -1;
    for(int i=0;i<numt;i++) {
      xsem_wait(&d.free_slots,__LINE__,__FILE__);
      d.buffer[(pindex++) % Buf_size] = r; 
      xsem_post(&d.data_items,__LINE__,__FILE__);
    }
    // wait for termination of threads
    for(int i=0;i<numt;i++)
      xpthread_join(t[i],NULL,__LINE__,__FILE__);
    // done
    words = d.full_words;
    pc_destroy(&d.free_slots,&d.data_items, &d.cons_m);
  }
  cout << "Conversion took " << difftime(time(NULL),start) << " wall clock seconds\n";  
  cout << "DA has size: " << dsize-dwords-w-1;
  cout << ". Dictionary words found: " << words << endl; 
}

// --------------------------------------------------------------------
// multithread construction of the final BWT from dict and parse 

// range in the suffix array of the dictionary 
typedef struct {
  long start;      // starting position in the dictionary 
  long end;        // end position in the dictionary
  long bwt_start;  // starting position in the output bwt
  long count;      // chars to be written to the output bwt;
} da_range;

// working data to be passed to each consumer thread
typedef struct {
  uint8_t *dict;       // dictionary
  uint_t *da, *eos;    // document array, eos positions
  int_t *suflen, *wlen; // suffix lengths, length of words 
  long dsize;          // size of dict[] da[] suflen[]  
  long dwords;         // number of words in the dictionary size of eos[] wlen[] 
  uint8_t *last;       // array of last symbols 
  uint32_t *ilist;     // inverted list 
  uint32_t *istart;    // starting positions inside inverted list 
  int w;               // window size
  da_range buffer[Buf_size]; // shared producer/consumer buffer 
  int cindex;                // consumer index in buffer
  pthread_mutex_t cons_m;    // mutex and semaphores 
  sem_t free_slots, data_items;
  int bwt_fd;                // file descriptor for the bwt output file  
  long full_words;           // output parameters, access with a mutex_consumer
  long easy_bwts; 
  long hard_bwts; 
} thread_data;


// write to the bwt all the characters preceding a given suffix
// doing a merge operation if necessary
static void write_chars_same_suffix(vector<uint32_t> &id2merge,  vector<uint8_t> &char2write, 
                                    uint32_t *ilist, uint32_t *istart,
                                    uint8_t *bwt, long &c, long &easy_bwts, long &hard_bwts)
{
  size_t numwords = id2merge.size(); // numwords dictionary words contains the same suffix
  bool samechar=true;
  for(size_t i=1;(i<numwords)&&samechar;i++)
    samechar = (char2write[i-1]==char2write[i]); 
  if(samechar) {
    for(size_t i=0; i<numwords; i++) {
      uint32_t s = id2merge[i];
      for(long j=istart[s];j<istart[s+1];j++)
        bwt[c++] = char2write[0];
      easy_bwts +=  istart[s+1]- istart[s]; 
    }
  }
  else {  // many words, many chars...     
    vector<SeqId> heap; // create heap
    for(size_t i=0; i<numwords; i++) {
      uint32_t s = id2merge[i];
      heap.push_back(SeqId(s,istart[s+1]-istart[s], ilist+istart[s], char2write[i]));
    }
    std::make_heap(heap.begin(),heap.end());
    while(heap.size()>0) {
      // output char for the top of the heap
      SeqId s = heap.front();
      bwt[c++] = s.char2write;
      hard_bwts += 1;
      // remove top 
      pop_heap(heap.begin(),heap.end());
      heap.pop_back();
      // if remaining positions, reinsert to heap
      if(s.next()) {
        heap.push_back(s);
        push_heap(heap.begin(),heap.end());
      }
    }
  }
}


static void *merge_body(void *v)
{
  thread_data *d = (thread_data *) v;

  long i, next, c, full_words=0, easy_bwts=0, hard_bwts=0;
  uint8_t *local_bwt = NULL;
  // main loop 
  while(true) {
    // --- get starting position from buffer 
    xsem_wait(&d->data_items,__LINE__,__FILE__);
    xpthread_mutex_lock(&d->cons_m,__LINE__,__FILE__);
    da_range r = d->buffer[d->cindex++ % Buf_size]; 
    xpthread_mutex_unlock(&d->cons_m,__LINE__,__FILE__);
    xsem_post(&d->free_slots,__LINE__,__FILE__);
    // exit if start is illegal
    if(r.start<0) break;
    // process range [start,end]
    local_bwt = (uint8_t *) realloc(local_bwt,r.count);
    assert(local_bwt!=NULL);
    for(c=0, i = r.start; i<r.end; i=next){
      // we are considering d[sa[i]....] belonging to da[i]
      next = i+1;  // prepare for next iteration  
      // discard if it is a small suffix 
      if(d->suflen[i]<=d->w) continue;
      uint32_t seqid = d->da[i]&0x7FFFFFFF;
      assert(seqid<d->dwords);
      // ----- simple case: the suffix is a full word 
      if(d->suflen[i]==d->wlen[seqid]) {
        full_words++;
        for(long j=d->istart[seqid];j<d->istart[seqid+1];j++) 
          local_bwt[c++] = d->last[d->ilist[j]];
        continue; // proceed with next i 
      }
      // ----- hard case: there can be a group of equal suffixes starting at i
      // save seqid and the corresponding char 
      vector<uint32_t> id2merge(1,seqid); 
      vector<uint8_t> char2write(1,d->dict[d->eos[seqid]-d->suflen[i]-1]);
      while(next<r.end && d->suflen[next]==d->suflen[i]) {
        seqid = d->da[next]&0x7FFFFFFF;
        if(d->da[next]&0x80000000u) {
          assert(d->suflen[next]!=d->wlen[seqid]);   // the lcp cannot be greater than suffixLen
          id2merge.push_back(seqid);           // sequence to consider
          char2write.push_back(d->dict[d->eos[seqid]-d->suflen[next]-1]);  // corresponding char
          next++;
        }
        else break;
      }
      write_chars_same_suffix(id2merge, char2write, d->ilist,d->istart,local_bwt,c,easy_bwts,hard_bwts);
    }
    assert(i==r.end);
    assert(c==r.count);
    // write local_bwt to file d->bwt_fd starting from position r.bwt_start
    c = 0; 
    while(r.count>0) {
      long written = pwrite(d->bwt_fd,local_bwt+c,r.count,r.bwt_start);
      if(written<0) die("pwrite error (1)");
      if(written>r.count) die("pwrite error (2)");
      r.count -= written;
      r.bwt_start += written;
      c += written;
    }
  }
  if(local_bwt!=NULL) free(local_bwt);
  xpthread_mutex_lock(&d->cons_m,__LINE__,__FILE__);
  d->easy_bwts += easy_bwts;  
  d->hard_bwts += hard_bwts;  
  d->full_words += full_words;  
  xpthread_mutex_unlock(&d->cons_m,__LINE__,__FILE__);
  return NULL;
}


// bwt construction from dictionary and parse using multiple threads
void bwt_multi(uint8_t *d, long dsize, // dictionary and its size  
         uint32_t *ilist, uint8_t *last, long psize, // ilist, last and their size 
         uint32_t *istart, long dwords, // starting point in ilist for each word and # words
         int w, char *name, int numt)   // window size and base name for output file
{  
  (void) psize; // used only in assertions
  assert(numt>0); 
  // compute sa and bwt of d and do some checking on them 
  uint_t *sa; int_t *lcp; 
  compute_dict_bwt_lcp(d,dsize,dwords,w,&sa,&lcp);
  // set d[0] ==0 as this is the EOF char in the final BWT
  assert(d[0]==Dollar);
  d[0]=0;

  // convert sa,lcp->da,suflen + bit
  sa2da(sa,lcp,d,dsize,dwords,w,numt);
  uint_t *da = sa + (dwords+w+1);
  uint_t *eos = sa+1;
  long dasize= dsize - (dwords+w+1);
  int_t *suflen = lcp + (dwords+w+1);
  int_t *wlen = lcp+1;
  lcp = NULL; sa = NULL; // make sure these are not used

  // init thread_data
  thread_data td;
  td.dict = d; td.dsize = dsize; td.dwords = dwords;
  td.suflen = suflen; td.wlen = wlen;
  td.da = da; td.eos = eos; td.w = w;
  td.last = last; td.ilist = ilist; td.istart=istart;
  td.full_words = td.easy_bwts = td.hard_bwts = 0;
  td.cindex=0;
  pc_init(&td.free_slots,&td.data_items,&td.cons_m); 
  td.bwt_fd = get_bwt_fd(name); // file descriptor of output bwt file

  // start consumer threads
  pthread_t t[numt];
  for(int i=0;i<numt;i++)
    xpthread_create(&t[i],NULL,merge_body,&td,__LINE__,__FILE__);

  // main loop: consider each entry in the DA[] of dict
  //uint8_t *bwt = get_mmaped_bwt(name);
  time_t  start = time(NULL);
  long written=0, entries=0;  
  long next, full_words=0;
  int pindex=0; da_range r = {0,0,0,0};
  for(long i=0; i< dasize; i=next ) {
    // ---- if a batch is ready write it to the prod/cons buffer
    if(entries >= Min_bwt_range) {
      r.start = r.end; r.end = i;
      r.bwt_start = written; r.count = entries;
      xsem_wait(&td.free_slots,__LINE__,__FILE__);
      td.buffer[pindex++ % Buf_size] = r;
      xsem_post(&td.data_items,__LINE__,__FILE__);
      written += entries; entries=0;
    }
    next = i+1;  // prepare for next iteration  
    // discard if it is a small suffix 
    if(suflen[i]<=w) continue;
    uint32_t seqid = da[i]&0x7FFFFFFF;
    assert(seqid<dwords);
    entries += istart[seqid+1]-istart[seqid];
    // ----- simple case: the suffix is a full word 
    if(suflen[i]==wlen[seqid]) {
      full_words++;
      continue; // proceed with next i 
    }
    // ----- hard case: there can be a group of equal suffixes starting at i
    while(next<dasize && suflen[next]==suflen[i]) {
      seqid = da[next]&0x7FFFFFFF;
      if(da[next]&0x80000000u) {
        assert(suflen[next]!=wlen[seqid]);   // the lcp cannot be greater than suffixLen
        entries += istart[seqid+1]-istart[seqid];
        next++;
      }
      else break;
    }
  }
  // write last range to pc buffer
  r.start = r.end; r.end = dasize;
  r.bwt_start = written; r.count = entries;
  xsem_wait(&td.free_slots,__LINE__,__FILE__);
  td.buffer[pindex++ % Buf_size] = r;
  xsem_post(&td.data_items,__LINE__,__FILE__);
  // terminate and join threads 
  r.start = -1; 
  for(int i=0;i<numt;i++) {
    xsem_wait(&td.free_slots,__LINE__,__FILE__);
    td.buffer[pindex++ % Buf_size] = r;
    xsem_post(&td.data_items,__LINE__,__FILE__);
  }
  for(int i=0;i<numt;i++)
    xpthread_join(t[i],NULL,__LINE__,__FILE__);

  assert(td.full_words==dwords);  
  cout << "Full words: " << td.full_words << endl;
  cout << "Easy bwt chars: " << td.easy_bwts << endl;
  cout << "Hard bwt chars: " << td.hard_bwts << endl;
  cout << "Generating the final BWT took " << difftime(time(NULL),start) << " wall clock seconds (" << numt <<" threads, range "<< Min_bwt_range<<")\n";    
  pc_destroy(&td.free_slots,&td.data_items,&td.cons_m);
  close(td.bwt_fd); // close bwt file
  delete[] lcp;
  delete[] sa;
}

// compute size of the bwt adding 1 to the input size
#if 0
static size_t get_bwt_size(char *name)
{
  FILE *f = fopen(name,"rb");
  if(f==NULL) die("Input file open");
  int e = fseek(f,0,SEEK_END);
  if(e<0) die("Input file seek");
  long s = ftell(f);
  if(s<0) die("Input file tell");
  if(fclose(f)!=0) die("Input file close");
  cerr << "input file size: " << s << endl;
  return 1 + s;
}
#endif

static int get_bwt_fd(char *name)
{
  // get final bwt size from the size of the input file    
  //!!size_t bwt_size= get_bwt_size(name);
  // open output file and map it to the bwt array 
  // FILE *fbwt = open_aux_file(name,"bwt","wb+");
  int bwt_fd = fd_open_aux_file(name,"bwt",O_CREAT|O_WRONLY|O_TRUNC);
  // make the BWT file of the correct size (otherwise mmap fails)
  //!!if(ftruncate(bwt_fd,bwt_size)<0) die("truncate failed");
  //uint8_t *bwt = (uint8_t *) mmap(NULL,bwt_size,PROT_READ|PROT_WRITE,MAP_SHARED,fileno(fbwt), 0);
  //if(bwt==MAP_FAILED) die("mmap failed");
  //fclose(fbwt); 
  return bwt_fd;
}

// initialize/destroy semaphores and mutex for producer/consumer 
static void pc_init(sem_t *free_slots, sem_t *data_items, pthread_mutex_t *m)
{
  xpthread_mutex_init(m,NULL,__LINE__,__FILE__);
  xsem_init(free_slots,0,Buf_size,__LINE__,__FILE__);
  xsem_init(data_items,0,0,__LINE__,__FILE__);
}  
  
static void pc_destroy(sem_t *free_slots, sem_t *data_items, pthread_mutex_t *m)
{
  xpthread_mutex_destroy(m,__LINE__,__FILE__);
  xsem_destroy(free_slots,__LINE__,__FILE__);
  xsem_destroy(data_items,__LINE__,__FILE__);
}  
  
  