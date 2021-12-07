#!/usr/bin/perl
#
use strict;
use warnings;

if (($#ARGV != 1) || ( ! -f "$ARGV[0]" ) || ( ! -f $ARGV[1])) {
  print "Usage: precount.pl srcfile tgtfile\n";
  print "    Gathers change statistics between src and tgt lines\n";
  print "    Assuming both represent full functions\n";
  exit(1);
}

open(my $src, "<", $ARGV[0]) || die "open $ARGV[0] failed: $!";
open(my $tgt, "<", $ARGV[1]) || die "open $ARGV[1] failed: $!";

my $error=0;
my $npairs=0;
my $nn0=0;
my $tn0=0;
my $n0n=0;
my $t0n=0;
my $n11=0;
my $nnn=0;
my $tnn=0;
my $n1n=0;
my $t1n=0;
my $nmn=0;
my $tmn=0;
my $nnm=0;
my $tnm=0;
while (<$src>) {
  $npairs++;
  my $sline=$_;
  my $tline=<$tgt> || die "src and tgt line counts don't match";
  $sline =~ s/\s+$//;
  $sline =~ s/\s\/\/<S2SV>//g;
  $sline =~ s/<S2SV_StartBug> //g;
  $sline =~ s/ <S2SV_EndBug>//g;
  $tline =~ s/\s+$//;
  $tline =~ s/\s\/\/<S2SV>//g;
  $tline =~ s/<S2SV_StartBug> //g;
  $tline =~ s/ <S2SV_EndBug>//g;
  $sline .= " <S2SV_null> <S2SV_null> <S2SV_null> <S2SV_null>";
  $tline .= " <S2SV_null> <S2SV_null> <S2SV_null> <S2SV_null>";
  my @stok = split / +/,$sline;
  my @ttok = split / +/,$tline;
  my $s=0;
  my $t=0;
  while ($s < scalar @stok -4) {
    if ($stok[$s] eq $ttok[$t]) {
      $s++;
      $t++;
      next;
    }
    for (my $i=1; $i <= 100; $i++) {
      for (my $h=0; $h <= $i+$i; $h++) {
        # checking small offsets first will be faster and more accurate
        my $k = ($h < $i) ? $i : $i*2-$h;
        my $j = ($h < $i) ? $h : $i;
        if (($s+$j+2 < scalar @stok) && ($t+$k+2 < scalar @ttok) && ($stok[$s+$j] eq $ttok[$t+$k]) && ($stok[$s+$j+1] eq $ttok[$t+$k+1]) && ($stok[$s+$j+2] eq $ttok[$t+$k+2])) {
          if ($j == 0) {
            $n0n++;
            $t0n+=$k;
          } elsif ($k == 0) {
            $nn0++;
            $tn0+=$j;
          } elsif (($j == 1) && ($k == 1)) {
            $n11++;
            $nnn++;
            $tnn+=$j;
            $n1n++;
            $t1n+=$k;
            $nmn++;
            $tmn+=$k;
            $nnm++;
            $tnm+=$j;
          } elsif ($j == $k) {
            $nnn++;
            $tnn+=$j;
            $nmn++;
            $tmn+=$k;
            $nnm++;
            $tnm+=$j;
          } elsif ($j == 1) {
            $n1n++;
            $t1n+=$k;
            $nmn++;
            $tmn+=$k;
            $nnm++;
            $tnm+=$j;
          } else {
            $nmn++;
            $tmn+=$k;
            $nnm++;
            $tnm+=$j;
          }
          $s+=$j+3;
          $t+=$k+3;
          $i=200;
          $h=500;
        }
      }
      if ($i == 100) {
        $error++;
        $s= scalar @stok;
      }
    }
  }
}
printf "%-30s %-10s   %-10s\n","","Average","Average";
printf "%-30s %-10s   %-10s\n","","Number Per","Value";
printf "%-30s %-10s   %-10s\n","Change Description","Sample","Of 'n'";
printf "%-28s %8.1f   %8.1f\n","Insert 'n' tokens",$n0n/$npairs,$t0n/$n0n;
printf "%-28s %8.1f   %8.1f\n","Delete 'n' tokens",$nn0/$npairs,$tn0/$nn0;
printf "%-28s %8.1f   %10s\n","Change 1 token",$n11/$npairs,"N/A  ";
printf "%-28s %8.1f   %8.1f\n","Change 'n' tokens",$nnn/$npairs,$tnn/$nnn;
printf "%-28s %8.1f   %8.1f\n","Change 1 token to 'n' tokens",$n1n/$npairs,$t1n/$n1n;
printf "%-28s %8.1f   %8.1f\n","Change 'm' to 'n' tokens",$nmn/$npairs,$tmn/$nmn;
printf "%-28s %8.1f   %8.1f\n","Change 'n' to 'm' tokens",$nnm/$npairs,$tnm/$nnm;
print "Numper of pairs: $npairs, Errors: $error\n";

close $src;
close $tgt;
