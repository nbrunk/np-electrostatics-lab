// This is main.
// This is Car Parrinello molecular dynamics for simulating dynamics of ions near a nanoparticle (NP) surface
// This will power app 2: nanosphere electrostatics lab. the app is part of the nanoparticle characterization framework.
// The framework is expected to launch nanorod electrostatics lab, nanodisc electrostatics lab etc. apps
// Problem : Compute the density profile of ions around a NP; and estimate a zeta potential or effective charge of the ion
/* Useful studies :	
		     1. Role of dielectric contrast
		     2. Role of valency of ions
		     3. Role of varying salt concentration
		     4. Role of NP charge
*/

/* @kadupitiya
 * e = E = 78.5 does simple MD; e != E invokes cpmd
 * these parameters produce good converged results that can be compared against the available data (which is plotted in the image sent separately):
 * -a 2.6775 -b 14.28 -e 2 -E 78.5 -V -60 -v 1 -g 1082 -m 6 -t 0.001 -s 10000 -p 100 -f 10 -M 6 -T 0.001 -k 0.0025 -q 0.001 -L 5 -l 5 -S 10000000 -P 100000 -F 100 -X 10000 -U 1000 -Y 500000 -W 1000000 -B 0.025
 * change valency v to 1 (red in image), 2 (green), 3 (blue); everything else can remain the same
 * m = M, k, t = T, q depend on g (the grid size); g depends on e & E, and a. higher the dielectric contrast, that is difference between e and E, higher g is needed to resolve the induced charge density.
 * the selection of these 5 parameters is made manually by monitoring energies, trial and error-- I spent a lot of time just doing that before submitting a useful run to get the ion density. i think there is a possibility of using ML to select these 5 CPMD parameters judiciously. we can pursue this tangentially to nanohub project; hoping it spirals into one of our collaborative hpc/ml projects.
 * the default parameters right now in boost are for a fast simulation that works. as you see they are different than the above parameters for different g etc.:
 * -a 2.6775 -b 14.28 -e 2 -E 78.5 -V -60 -v 1 -g 132 -m 1 -t 0.001 -s 10000 -p 100 -f 10 -M 1 -T 0.001 -k 0.01 -q 1 -L 5 -l 5 -S 50000 -P 10000 -F 100 -X 1000 -U 1000 -Y 10000 -W 10000 -B 0.1
 * the quick_check_polarized_data has a short run from these paramaters for you to perform a quick check against any code changes, if you want.
 * the following is slightly slower but produces a better profile (still far from converged):
 * -a 2.6775 -b 14.28 -e 2 -E 78.5 -V -60 -v 1 -g 132 -m 1 -t 0.001 -s 10000 -p 100 -f 10 -M 1 -T 0.001 -k 0.01 -q 1 -L 5 -l 5 -S 200000 -P 100000 -F 100 -X 10000 -U 10000 -Y 100000 -W 10000 -B 0.1
 * for cpmd, successful simulation demands more than energy conservation:
 * 	1. _ind_*.dat files in verifiles should roughly match the _cpmd_*.dat files in computedfiles
 * 	2. total_induced_charge.dat in outfiles should be very close to 0
 * 	3. track_deviation.dat in outfiles should be small (< 1) and stable-- usually this will happen if the above 2 criteria hold; see also average deviation before R in the output at the end-- should be small
 * 	4. and of course, R should be small like before (could be a bit higher than normal MD)
 */

#include <boost/program_options.hpp>
#include "functions.h"
#include "precalculations.h"
#include "NanoParticleDisk.h"
#include "NanoParticleSphere.h"

//MPI boundary parameters
unsigned int lowerBoundIons;
unsigned int upperBoundIons;
unsigned int sizFVecIons;
unsigned int extraElementsIons;
unsigned int lowerBoundMesh;
unsigned int upperBoundMesh;
unsigned int sizFVecMesh;
unsigned int extraElementsMesh;
mpi::environment env;
mpi::communicator world;

vector<int> condensedIonsPerStep; // Number of condensed ions per step (after equilibrium) at specified frequency

using namespace boost::program_options;

int main(int argc, char *argv[]) {

    // Electrostatic system variables
    double radius;        // radius of the dielectric sphere
    double ein;            // permittivity of inside medium
    double eout;            // permittivity of outside medium
    int counterion_valency;    // counterion valency (positive by convention)
    double counterion_diameter;    // counterion diameter
    int salt_valency_in;        // salt valency inside
    int salt_valency_out;        // salt valency outside
    double salt_conc_in;        // salt concentration outside	(enter in M)
    double salt_conc_out;        // salt concentration outside	(enter in M)
    double saltion_diameter_in;    // inside salt ion diameter	(positive and negative ions assumed to have same diameter at this point)
    double saltion_diameter_out;  // outside salt ion diameter	(positive and negative ions assumed to have same diameter at this point)
    double real_T;            // temperature at which the system of ions is
    double nanoparticle_bare_charge; // bare charge of the NP

    // Simulation related variables
    int total_gridpoints;        // total number of grid points that discretize the continuous interface
    double Q;            // thermostat mass required to generate canonical ensemble
    double fake_T;            // fake temperature useful for Car Parrinello dynamics
    double fake_Q;            // fake thermostat mass required to generate canonical ensemble for fake degrees
    unsigned int chain_length_real;
    unsigned int chain_length_fake;
    double box_radius;        // simulation box size, measured as radius in case of a sphere
    double bin_width_R;        // width of the bins (in R direction) used to compute density profiles
    double bin_width_Z;        // width of the bins (in Z direction) used to compute density profiles
    CONTROL fmdremote;        // remote control for fmd
    CONTROL cpmdremote;        // remote control for cpmd


    // Different parts of the system
    //INTERFACE nanoparticle;        // interface(s)
    vector<PARTICLE> counterion;    // counterions
    vector<PARTICLE> saltion_in;    // salt ions inside
    vector<PARTICLE> saltion_out;    // salt ions outside
    vector<PARTICLE> ion;        // all ions in the system
    vector<VERTEX> s;        // all vertices

    // Analysis
    string np_shape; // np shape
    NanoParticle *nanoParticle;
    VECTOR3D np_pos(0, 0, 0);

    // Get input values from the user
    options_description desc("Usage:\nrandom_mesh <options>");
    desc.add_options()
            ("help,h", "print usage message")
            ("radius,a", value<double>(&radius)->default_value(2.6775),
             "sphere radius")                // enter in nanometers
            ("epsilon_in,e", value<double>(&ein)->default_value(78.5), "dielectric const inside")
            ("epsilon_out,E", value<double>(&eout)->default_value(78.5), "dielectric const outside")
            ("counterion_valency,v", value<int>(&counterion_valency)->default_value(1), "counterion valency")
            ("nanoparticle_charge,V", value<double>(&nanoparticle_bare_charge)->default_value(-60),
             "nanoparticle charge")
            ("salt_valency_in,z", value<int>(&salt_valency_in)->default_value(1), "salt valency inside")
            ("salt_valency_out,Z", value<int>(&salt_valency_out)->default_value(1), "salt valency outside")
            ("salt_conc_in,c", value<double>(&salt_conc_in)->default_value(0.0), "salt concentration inside")
            ("salt_conc_out,C", value<double>(&salt_conc_out)->default_value(0.0), "salt concentration outside")
            ("counterion_diameter,x", value<double>(&counterion_diameter)->default_value(0.357),
             "counterion diameter")            // enter in nanometers
            ("saltion_diameter_in,d", value<double>(&saltion_diameter_in)->default_value(0.357),
             "salt ion diameter inside")        // enter in nanometers
            ("saltion_diameter_out,D", value<double>(&saltion_diameter_out)->default_value(0.357),
             "salt ion diameter outside")        // enter in nanometers
            ("total_gridpoints,g", value<int>(&total_gridpoints)->default_value(132), "gridpoints")
            ("thermostat_mass,Q", value<double>(&Q)->default_value(1.0), "thermostat mass")
            ("chain_length_real,L", value<unsigned int>(&chain_length_real)->default_value(5),
             "chain length for real system: enter L+1 if you want L thermostats")
            ("fake_temperature,k", value<double>(&fake_T)->default_value(0.01), "fake temperature")
            ("fake_thermostat_mass,q", value<double>(&fake_Q)->default_value(1.0), "fake thermostat mass")
            ("chain_length_fake,l", value<unsigned int>(&chain_length_fake)->default_value(5),
             "chain length for fake system: enter L+1 if you want L thermostats")
            ("box_radius,b", value<double>(&box_radius)->default_value(14.28),
             "simulation box radius")        // enter in nanometers
            ("bin_width_R,R", value<double>(&bin_width_R)->default_value(0.1), "bin width R")
            ("bin_width_Z,B", value<double>(&bin_width_Z)->default_value(0.2), "bin width Z")
            ("anneal_fmd,A", value<char>(&fmdremote.anneal)->default_value('n'), "anneal in fmd on?")
            ("fmd_fake_mass,m", value<double>(&fmdremote.fakemass)->default_value(1.0), "fmd fake mass")
            ("cpmd_fake_mass,M", value<double>(&cpmdremote.fakemass)->default_value(1.0), "cpmd fake mass")
            ("fmd_timestep,t", value<double>(&fmdremote.timestep)->default_value(0.001), "time step used in fmd")
            ("cpmd_timestep,T", value<double>(&cpmdremote.timestep)->default_value(0.001), "time step used in cpmd")
            ("fmd_steps,s", value<int>(&fmdremote.steps)->default_value(10000), "steps used in fmd")
            ("cpmd_steps,S", value<int>(&cpmdremote.steps)->default_value(50000), "steps used in cpmd")
            ("fmd_eqm,p", value<int>(&fmdremote.hiteqm)->default_value(100), "production begin (fmd)")
            ("cpmd_eqm,P", value<int>(&cpmdremote.hiteqm)->default_value(10000), "production begin (cpmd)")
            ("fmd_freq,f", value<int>(&fmdremote.freq)->default_value(10), "sample frequency (fmd)")
            ("cpmd_freq,F", value<int>(&cpmdremote.freq)->default_value(100), "sample frequency (cpmd)")
            ("fmd_verify,y", value<int>(&fmdremote.verify)->default_value(0), "verify (fmd)")
            ("cpmd_verify,Y", value<int>(&cpmdremote.verify)->default_value(10000), "verify (cpmd)")
            ("cpmd_writedata,U", value<int>(&cpmdremote.writedata)->default_value(1000), "write data files")
            ("cpmd_extra_compute,X", value<int>(&cpmdremote.extra_compute)->default_value(1000),
             "compute additional (cpmd)")
            ("cpmd_writedensity,W", value<int>(&cpmdremote.writedensity)->default_value(10000), "write density files")
            ("np_shape,G", value<string>(&np_shape)->default_value("Sphere"), "nanoparticle shape")
            ("verbose,I", value<bool>(&cpmdremote.verbose)->default_value(true),
             "verbose true: provides detailed output");

    variables_map vm;
    store(parse_command_line(argc, argv, desc), vm);
    notify(vm);
    if (vm.count("help")) {
        if (world.rank() == 0)
            std::cout << desc << "\n";
        return 0;
    }

    if (world.rank() == 0)
        cout << "\nProgram starts\n";

    int numOfNodes = world.size();
    if (world.rank() == 0) {
#pragma omp parallel default(shared)
        {
            if (omp_get_thread_num() == 0) {
                printf("The app comes with MPI and OpenMP (Hybrid) parallelization)\n");
                printf("Number of MPI processes used %d\n", numOfNodes);
                printf("Number of OpenMP threads per MPI process %d\n", omp_get_num_threads());
                printf("Make sure that number of grid points / ions is greater than %d\n",
                       omp_get_num_threads() * numOfNodes);
            }
        }
    }

    //serve different NP for density bin
    if (np_shape.compare("Sphere") == 0) {
        //Sphere
        vector<BinShell> bin_pos, bin_neg;
        vector<double> sample_density_pos, sample_density_neg;
        NanoParticleSphere np("Sphere", bin_pos, bin_neg, bin_width_R, ion, sample_density_pos, sample_density_neg, 0,
                              0, cpmdremote,
                              np_pos, radius / unitlength, ein, eout,
                              nanoparticle_bare_charge);

        nanoParticle = &np;

    } else {
        //disk

        vector<vector<double> > density_pos, density_neg;
        vector<vector<BinRing> > bin_disk_pos, bin_disk_neg;
        NanoParticleDisk np("Disk", bin_disk_pos, bin_disk_neg, bin_width_R, bin_width_Z, ion, density_pos, density_neg,
                            0, 0, cpmdremote,
                            np_pos, radius / unitlength, ein, eout,
                            nanoparticle_bare_charge);

        nanoParticle = &np;

    }

    // Set up the system
    real_T = 1;

    // set temperature
    // make interface
    nanoParticle->set_up(salt_conc_in, salt_conc_out, salt_valency_in, salt_valency_out, total_gridpoints,
                         box_radius / unitlength);    // set up properties inside and outside the interface

    // If running a standard simulation (charged particle and/or with salt) populate the ions:
    if (abs(nanoparticle_bare_charge) > 0)
        nanoParticle->put_counterions(counterion, counterion_valency, counterion_diameter, ion);                    // put counterions	Note: ion contains all ions
    if (salt_conc_in > 0)
        nanoParticle->put_saltions_inside(saltion_in, salt_valency_in, salt_conc_in, saltion_diameter_in, ion);                // put salt ions inside
    if (salt_conc_out > 0)
        nanoParticle->put_saltions_outside(saltion_out, salt_valency_out, salt_conc_out, saltion_diameter_out, ion);            // put salt ions outside

    //  If the charge is set to zero (for testing), insert test two test ions at chosen positions:
    /*if (nanoparticle_bare_charge == 0)
    {
        ion.push_back(PARTICLE(int(ion.size()) + 1, counterion_diameter, counterion_valency, counterion_valency * 1.0, 1.0, eout, VECTOR3D(5 / unitlength,0,0)));
        ion.push_back(PARTICLE(int(ion.size()) + 1, counterion_diameter, counterion_valency, counterion_valency * 1.0, 1.0, eout, VECTOR3D(6 / unitlength,0,0)));
    }*/

    nanoParticle->discretize(s,radius / unitlength);                                // discretize interface

    // if dielectric environment inside and outside NP are different, NPs get polarized
    if (nanoParticle->ein == nanoParticle->eout)
        nanoParticle->POLARIZED = false;
    else
        nanoParticle->POLARIZED = true;

    if (world.rank() == 0) {
        if (nanoParticle->POLARIZED)
            cout << "NP is polarized " << endl;
        else
            cout << "NP is not polarized " << endl;
    }

    nanoParticle->RANDOMIZE_ION_FEATURES = false;

    // NOTE: sizing the arrays employed in precalculate functions
    for (unsigned int k = 0; k < s.size(); k++) {
        s[k].presumgwEw.resize(s.size());
        s[k].presumgEwEq.resize(s.size());
        s[k].presumgEwEw.resize(s.size());
        s[k].presumfwEw.resize(s.size());
        s[k].presumfEwEq.resize(s.size());
        s[k].presumhEqEw.resize(s.size());
    }

    // could only do precalculate if CPMD
    if (nanoParticle->POLARIZED)
        precalculate(s, nanoParticle);                        // precalculate

    for (unsigned int k = 0; k < s.size(); k++)               // get polar coordinates for the vertices
        s[k].get_polar();

    //make bins
    nanoParticle->make_bins();

    if (world.rank() == 0) {
        // output to screen the parameters of the problem
        cout << "\n";
        if (cpmdremote.verbose)
            cout << "Reduced units: scalefactor entering in Coloumb interaction is " << scalefactor << endl;
        cout << "Units : length (cms) " << unitlength * pow(10.0, -7) << " | " << "energy(ergs) " << unitenergy
             << " | " << "mass(g) " << unitmass << " | " << "time(s) " << unittime << endl;
        cout << "Radius of the dielectric sphere (interface) " << nanoParticle->radius << endl;
        cout << "Nanoparticle charge " << nanoParticle->bare_charge << endl;
        cout << "Permittivity inside " << nanoParticle->ein << endl;
        cout << "Permittivity outside " << nanoParticle->eout << endl;
        if (cpmdremote.verbose)
            cout << "Contrast strength "
                 << 2 * (nanoParticle->eout - nanoParticle->ein) / (nanoParticle->eout + nanoParticle->ein)
                 << endl;
        cout << "Counterion valency " << counterion_valency << endl;
        if (cpmdremote.verbose) {
            cout << "Salt ion valency inside " << salt_valency_in << endl;
            cout << "Salt ion valency outside " << salt_valency_out << endl;
            cout << "Counterion diameter " << counterion_diameter / unitlength << endl;
            cout << "Salt ion diameter inside " << saltion_diameter_in / unitlength << endl;
            cout << "Salt ion diameter outside " << saltion_diameter_out / unitlength << endl;
            cout << "Salt concentration inside " << salt_conc_in << endl;
            cout << "Salt concentration outside " << salt_conc_out << endl;
            cout << "Debye length inside " << nanoParticle->inv_kappa_in << endl;
            cout << "Debye length outside " << nanoParticle->inv_kappa_out << endl;
            cout << "Mean separation inside " << nanoParticle->mean_sep_in << endl;
            cout << "Mean separation outside " << nanoParticle->mean_sep_out << endl;
        }
        cout << "Simulation box (spherical) radius " << nanoParticle->box_radius << endl;
        cout << "Number of counterions " << counterion.size() << endl;
        if (cpmdremote.verbose) {
            cout << "Number of salt ions inside " << saltion_in.size() << endl;
            cout << "Number of salt ions outside " << saltion_out.size() << endl;
            cout << "Number of points discretizing the interface " << s.size() << endl;
            //cout << "Binning width (uniform) " << bin_disk[0][0].width_R << endl;
            nanoParticle->printBinSize();
        }
    }
    // write to files
    // initial configuration
    ofstream initial_configuration("outfiles/initialconfig.dat");
    if (world.rank() == 0)
        for (unsigned int i = 0; i < ion.size(); i++)
            initial_configuration << "ion" << setw(5) << ion[i].id << setw(15) << "charge" << setw(5) << ion[i].q
                                  << setw(15) << "position" << setw(15) << ion[i].posvec << endl;
    initial_configuration.close();

    // initial density
    nanoParticle->compute_initial_density_profile();

    // some calculations before simulation begins
    if (world.rank() == 0)
        cout << "Total charge inside the sphere " << nanoParticle->total_charge_inside(ion) << endl;

    // NEW NOTE : resizing the member arrays Gion and gradGion to store dynamic precalculations in fmd and cpmd force routines
    for (unsigned int k = 0; k < s.size(); k++) {
        s[k].Gion.resize(ion.size());
        s[k].gradGion.resize(ion.size());
    }

    //MPI Boundary calculation for ions
    unsigned int rangeIons = ion.size() / world.size() + 1.5;
    lowerBoundIons = world.rank() * rangeIons;
    upperBoundIons = (world.rank() + 1) * rangeIons - 1;
    extraElementsIons = world.size() * rangeIons - ion.size();
    sizFVecIons = upperBoundIons - lowerBoundIons + 1;
    if (world.rank() == world.size() - 1) {
        upperBoundIons = ion.size() - 1;
        sizFVecIons = upperBoundIons - lowerBoundIons + 1 + extraElementsIons;
    }
    if (world.size() == 1) {
        lowerBoundIons = 0;
        upperBoundIons = ion.size() - 1;
    }

    //MPI Boundary calculation for meshPoints
    unsigned int rangeMesh = s.size() / world.size() + 1.5;
    lowerBoundMesh = world.rank() * rangeMesh;
    upperBoundMesh = (world.rank() + 1) * rangeMesh - 1;
    extraElementsMesh = world.size() * rangeMesh - s.size();
    sizFVecMesh = upperBoundMesh - lowerBoundMesh + 1;
    if (world.rank() == world.size() - 1) {
        upperBoundMesh = s.size() - 1;
        sizFVecMesh = upperBoundMesh - lowerBoundMesh + 1 + extraElementsMesh;
    }
    if (world.size() == 1) {
        lowerBoundMesh = 0;
        upperBoundMesh = s.size() - 1;
    }

    for (unsigned int k = 0; k < s.size(); k++) {
        s[k].w = 0.0;                                // Initialize fake degree value		(unconstrained)
        s[k].wmean = 0.0;
    }

    // Fictitious molecular dynamics
    if (nanoParticle->POLARIZED) {
        if (world.rank() == 0)
            cout << "Polarized charges detected; simulation will proceed using dynamical optimization framework (CPMD)"
                 << endl;
        fmd(s, ion, nanoParticle, fmdremote, cpmdremote);
    } else if (world.rank() == 0)
        cout << "no induced charges; simulation will proceed using simple MD" << endl;

    ofstream induced_density("outfiles/induced_density.dat");
    if (world.rank() == 0)
        // result of fmd
        for (unsigned int k = 0; k < s.size(); k++)
            induced_density << k + 1 << setw(15) << s[k].theta << setw(15) << s[k].phi << setw(15) << s[k].w << setw(15)
                            << s[k].wmean << endl;

    induced_density.close();
    // prepare for cpmd : make real and fake baths

    vector<THERMOSTAT> real_bath;
    if (chain_length_real == 1)
        real_bath.push_back(THERMOSTAT(0, real_T, 3 * ion.size(), 0.0, 0, 0));
    else {
        real_bath.push_back(THERMOSTAT(Q, real_T, 3 * ion.size(), 0, 0, 0));
        while (real_bath.size() != chain_length_real - 1)
            real_bath.push_back(THERMOSTAT(Q / (3 * ion.size()), real_T, 1, 0, 0, 0));
        real_bath.push_back(THERMOSTAT(0, real_T, 3 * ion.size(), 0.0, 0,
                                       0));            // finally, the coding trick: dummy bath (dummy bath always has zero mass)
    }

    vector<THERMOSTAT> fake_bath;
    if (!nanoParticle->POLARIZED)
        fake_T = 0;
//K is used for thermostat fake_T is k
    if (chain_length_fake == 1)
        fake_bath.push_back(THERMOSTAT(0, fake_T, s.size(), 0.0, 0, 0));
    else {
        fake_bath.push_back(THERMOSTAT(fake_Q, fake_T, s.size(), 0, 0, 0));
        while (fake_bath.size() != chain_length_fake - 1)
            fake_bath.push_back(THERMOSTAT(fake_Q / s.size(), fake_T, 1, 0, 0, 0));
        fake_bath.push_back(THERMOSTAT(0, fake_T, s.size(), 0.0, 0,
                                       0));            // finally, the coding trick: dummy bath (dummy bath always has zero mass)
    }
    if (world.rank() == 0 && cpmdremote.verbose) {
        cout << "Number of chains for real system" << setw(3) << real_bath.size() - 1 << endl;
        cout << "Number of chains for fake system" << setw(3) << fake_bath.size() - 1 << endl;
    }

    // Car-Parrinello Molecular Dynamics
    cpmd(ion, s, nanoParticle, real_bath, fake_bath, fmdremote, cpmdremote);

    if (world.rank() == 0) {
        // Post simulation analysis (useful for short runs, but performed otherwise too)
        if (cpmdremote.verbose)
            cout << "MD trust factor R (should be < 0.05) is " << compute_MD_trust_factor_R(cpmdremote.hiteqm) << endl;
        if (nanoParticle->POLARIZED && cpmdremote.verbose)
            cout << "MD trust factor RV (should be < 0.15) is " << compute_MD_trust_factor_R_v(cpmdremote.hiteqm)
                 << endl;
        //auto_correlation_function();
        cout << "Program ends" << endl;
        cout << endl;
    }
    return 0;
}
// End of main
