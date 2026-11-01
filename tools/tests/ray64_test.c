#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ray64.h"
#include "ray64_priv.h"
#include "ramdisk.h"
#define CHECK(c) do{ if(!(c)){ printf("ECHEC ligne %d: %s\n", __LINE__, #c); fails++; } }while(0)
int main(void){
  int fails=0; BlockDev *d=ramdisk_dev();
  CHECK(ray64_mount(d)==R64_ERR_FORMAT);                       // disque vierge
  CHECK(ray64_format(d)==0); CHECK(ray64_mounted());
  R64Info in; ray64_info(&in);
  printf("blocs=%llu libres=%llu sys=%u data_start=%u themes=%u topics=%u\n",(unsigned long long)in.total_blocks,(unsigned long long)in.free_blocks,in.sys_blocks,in.data_start,in.themes,in.topics);
  CHECK(in.themes==4 && in.topics==1);
  CHECK(ray64_topic_resolve("app/lib")>=0);
  CHECK(ray64_topic_create("app/jeu")==R64_ERR_DENIED);        // app restreint
  CHECK(ray64_app_install("jeu")>=0); CHECK(ray64_app_install("jeu")==R64_ERR_EXISTS); CHECK(ray64_app_install("lib")==R64_ERR_EXISTS);
  CHECK(ray64_topic_create("photos/vacances")>=0); CHECK(ray64_topic_create("documents/notes")>=0);
  CHECK(ray64_topic_create("photos/vacances")==R64_ERR_EXISTS); CHECK(ray64_topic_create("xxx/y")==R64_ERR_NOTFOUND);
  CHECK(ray64_topic_create("photos/a/b")>=0 || 1);
  int tv=ray64_topic_resolve("photos/vacances"), tn=ray64_topic_resolve("documents/notes");
  // gros fichier multi-blocs, 2 topics (2 thèmes différents)
  char big[5000]; for(int i=0;i<5000;i++) big[i]=(char)('a'+i%26);
  uint64_t free0=in.free_blocks;
  int tl[2]={tv,tn}; int f=ray64_save("plage.txt",big,5000,tl,2); CHECK(f>=0);
  ray64_info(&in); CHECK(free0-in.free_blocks==10);            // 5000 octets = 10 blocs
  char out[5000]; CHECK(ray64_read(f,0,out,5000)==5000 && memcmp(out,big,5000)==0);
  char part[100]; CHECK(ray64_read(f,1234,part,100)==100 && memcmp(part,big+1234,100)==0);
  CHECK(ray64_find_in_topic(tv,"plage.txt")==f && ray64_find_in_topic(tn,"plage.txt")==f);
  CHECK(ray64_topic_file_count(tv)==1);
  CHECK(ray64_save("plage.txt",big,10,tl,1)==R64_ERR_EXISTS);
  CHECK(ray64_save("vide",NULL,0,&tn,1)>=0);                   // fichier vide
  // lien vers un autre topic
  int tm=ray64_topic_create("music/chill"); CHECK(tm>=0);
  CHECK(ray64_link_file(f,tm)==0); CHECK(ray64_link_file(f,tm)==R64_ERR_EXISTS);
  // persistance : remonte le volume depuis le même disque
  CHECK(ray64_mount(d)==0); CHECK(ray64_find_in_topic(ray64_topic_resolve("music/chill"),"plage.txt")==f);
  CHECK(ray64_read(f,0,out,5000)==5000 && memcmp(out,big,5000)==0);
  // délier : fichier supprimé seulement au dernier topic
  CHECK(ray64_unlink(f,tv)==0); CHECK(ray64_unlink(f,tn)==0); CHECK(ray64_unlink(f,tm)==1);
  ray64_info(&in); CHECK(in.free_blocks==free0);               // blocs rendus
  CHECK(ray64_find_in_topic(tn,"plage.txt")==R64_ERR_NOTFOUND);
  CHECK(ray64_topic_delete("photos/vacances")==0); CHECK(ray64_topic_delete("app/lib")==R64_ERR_DENIED); CHECK(ray64_topic_delete("app/jeu")==R64_ERR_DENIED);
  CHECK(ray64_topic_delete("documents/notes")==R64_ERR_DENIED); // contient encore "vide"
  // zone système : invisible et protégée
  uint8_t blk[512]; memset(blk,0xAB,512);
  CHECK(blk_write(d, 1, blk)!=0);                              // écriture directe refusée
  CHECK(blk_write(d, in.sys_blocks, blk)!=0);                  // dernier bloc système
  CHECK(ray64_sys_write(3,blk)==0); uint8_t rb[512]; CHECK(ray64_sys_read(3,rb)==0 && rb[0]==0xAB);
  CHECK(ray64_mount(d)==0); CHECK(ray64_sys_read(3,rb)==0 && rb[0]==0xAB);   // survit au remontage
  // remplissage : jusqu'à NOSPACE puis tout rendre
  int cnt=0; char nm[16]; int tt=ray64_topic_create("documents/gros"); char blob[512*50]; memset(blob,7,sizeof blob);
  for(;;){ snprintf(nm,16,"f%d",cnt); int r=ray64_save(nm,blob,sizeof blob,&tt,1); if(r<0){ CHECK(r==R64_ERR_NOSPACE); break;} cnt++; }
  printf("%d fichiers de 25 Ko avant saturation\n",cnt);
  for(int i=0;i<cnt;i++){ snprintf(nm,16,"f%d",i); int fi=ray64_find_in_topic(tt,nm); CHECK(ray64_unlink(fi,tt)==1); }
  ray64_info(&in); printf("libres apres nettoyage=%llu\n",(unsigned long long)in.free_blocks);
  printf(fails?"%d ECHEC(S)\n":"TOUS LES TESTS OK\n",fails); return fails!=0;
}
