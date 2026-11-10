int nativeBundle(const char *);
int nativeReject(const char *);
int main(int argc,char **argv) {
 return argc==3 ? nativeReject(argv[1]) : argc==2 ? nativeBundle(argv[1]) : 2;
}
