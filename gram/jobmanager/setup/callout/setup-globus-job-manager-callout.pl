use Grid::GPT::Setup;
use Getopt::Long;
use English;
use File::Path;

if(!&GetOptions("nonroot|d:s","help|h","force|f")) 
{
    usage(1);
}

if(defined($opt_help))
{
    usage(0);
}

my $target_dir;

my $metadata =
    new Grid::GPT::Setup(package_name => "globus_gram_job_manager_setup_callout");

my $globusdir = $ENV{GLOBUS_LOCATION};
my @libs = <$globusdir/lib/libglobus_gram_job_manager_callout_*.a>;
my $config = "";
my $found = 0;

if(@libs > 1)
{
    die("Error determining flavor of callout library. More than one flavor installed\n");
} 

$libs[0] =~ s/\.a$//;

if(defined($opt_nonroot))
{
    if($opt_nonroot eq "") 
    {
	$target_dir = $globusdir . "/etc/";
    } 
    else 
    {
	$target_dir = "$opt_nonroot";
    }
}
else
{
   $target_dir = "/etc/grid-security";
}

open(CONF, "+>> $target_dir/gsi-authz.conf") ||
    die("Error while trying to open $target_dir/gsi-authz.conf. Check your permissions\n");

while(<CONF>)
{
    if($_ =~ /^globus_gram_jobmanager_authz.*/)
    {
        $found = 1;
        if(defined($opt_force))
        {
            $_ = "globus_gram_jobmanager_authz $libs[0] globus_gram_callout\n";
        }
        else
        {
            print STDERR "Warning: Configuration file already has a entry for the GRAM authorization\n callout. To overwrite re-run this setup script with the -force option\n";
        }
    }
    
    $config .= "$_";
}
    
if($found == 0)
{
    $config .= "globus_gram_jobmanager_authz $libs[0] globus_gram_callout\n";
}

close(CONF);

open(CONF, "> $target_dir/gsi-authz.conf") ||
    die("Error while trying to open $target_dir/gsi-authz.conf. Check your permissions\n");
    
print CONF "$config";

close(CONF);


if($? == 0)
{
    $metadata->finish();
}
else
{
    print STDERR "Error creating setting up the GRAM authorization callout.\n";
}

sub usage
{
    my $ex = shift;
    print "Usage: setup-globus-job-manager-callout [options]\n".
          "Options:  [--nonroot|-d[=path]]\n".
	  "          [--help|-h]\n";
    exit $ex;
}
