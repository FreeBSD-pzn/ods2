--- direct.c.orig	2001-08-31 22:01:07.000000000 +0400
+++ direct.c	2018-01-17 10:37:36.799439000 +0300
@@ -136,12 +136,15 @@
         register char sch = *name;
         if (sch != '*') {
             register char ech = *entry;
-                if (sch != ech) if (toupper(sch) != toupper(ech))
-                    if (sch == '%') {
-                        percent = MAT_NE;
-                    } else {
-                        break;
-                    }
+                if (sch != ech) {
+                   if (toupper(sch) != toupper(ech)) {
+                      if (sch == '%') {
+                          percent = MAT_NE;
+                      } else {
+                          break;
+                      }
+                   }
+                }
         } else {
             break;
         }
@@ -294,7 +297,7 @@
         struct VIOC *newvioc;
         unsigned newblk = eofblk + 1;
         direct_splits++;
-        printf("Splitting record... %d %d\n",dr,de);
+        printf("Splitting record... %p %p\n",dr,de);
         if (newblk > fcb->hiblock) {
             printf("I can't extend a directory yet!!\n");
             exit(0);
@@ -353,7 +356,7 @@
             register unsigned reclen = (dr->dir$namecount +
                                         sizeof(struct dir$rec)) & ~1;
             register struct dir$rec *nbr = (struct dir$rec *) newbuf;
-            printf("Super split %d %d\n",dr,de);
+            printf("Super split %p %p\n",dr,de);
             memcpy(newbuf,buffer,reclen);
             memcpy(newbuf + reclen,de,((char *) nr - (char *) de) + 2);
             nbr->dir$size = VMSWORD(reclen + ((char *) nr - (char *) de) - 2);
