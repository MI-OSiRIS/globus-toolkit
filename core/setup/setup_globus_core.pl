my $gpath = $ENV{GPT_LOCATION};
if (!defined($gpath))
{
  $gpath = $ENV{GLOBUS_LOCATION};

}
if (!defined($gpath))
{
   die "GPT_LOCATION or GLOBUS_LOCATION needs to be set before running this script"
}

@INC = (@INC, "$gpath/lib/perl");

require Grid::GPT::Setup;

my $metadata = new Grid::GPT::Setup(package_name => "globus_core_setup");

my $globusdir = $ENV{GLOBUS_LOCATION};
my $setupdir = "$globsdir/setup/globus/";
my $result = system("$setupdir/findshelltools");

open(TOOLS, "$setupdir/globus_sh_tools");

my ($perl_location, $sh_location);

while (<TOOLS>) {
  m!PERL=(\S+)! && do { $perl_location = $1 };
  m!SH=(\S+)! && do { $sh_location = $1 };
}

$result = system("ln -s $perl_location $globusdir/bin");
$result = system("ln -s $sh_location $globusdir/bin");

for my $f ('globus-script-initializer', 'globus-sh-tools.sh')
{
    $result = system("cp $f $globusdir/libexec");
}




$metadata->finish();

