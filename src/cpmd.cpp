// This is Car-Parrinello molecular dynamics (CPMD)

#include "functions.h"

extern vector<int> condensedIonsPerStep;

void cpmd(vector <PARTICLE> &ion, vector <VERTEX> &s, NanoParticle *nanoParticle, vector <THERMOSTAT> &real_bath,
          vector <THERMOSTAT> &fake_bath, CONTROL &fmdremote, CONTROL &cpmdremote) {


    // Part I : Initialize and set up
    for (unsigned int k = 0; k < s.size(); k++) {
        s[k].mu = cpmdremote.fakemass * s[k].a * s[k].a;                // fake degree masses assigned
        s[k].w = s[k].wmean;                    // fake degree positions initialized
    }
    initialize_fake_velocities(s, fake_bath, nanoParticle);
    long double sigma = constraint(s, ion, nanoParticle);            // constraint evaluated
    for (unsigned int k = 0; k < s.size(); k++)
        s[k].w = s[k].w - sigma / (s[k].a * s.size());        // constraint satisfied
    long double sigmadot = dotconstraint(s);                // time derivative of the constraint evaluated
    for (unsigned int k = 0; k < s.size(); k++)
        s[k].vw = s[k].vw - sigmadot / (s[k].a * s.size());        // time derivative of constraint satisfied
    // particle positions initialized already, before fmd
    initialize_particle_velocities(ion, real_bath, nanoParticle);        // particle velocities initialized
    // forces on particles and fake degrees initialized
    for_cpmd_calculate_force(s, ion, nanoParticle);
    long double particle_ke = particle_kinetic_energy(ion);        // compute initial particle kinetic energy
    long double fake_ke = fake_kinetic_energy(s);            // compute initial fake kinetic energy
    double potential_energy = energy_functional(s, ion, nanoParticle);    // Compute initial potential energy

    if (world.rank() == 0) {
        // Output cpmd essentials
        cout << "\n";
        cout << "Dynamical Optimization in the simulation (CPMD) is" << " on " << endl;
        if (cpmdremote.verbose) {
            cout << "Mass assigned to the fake degrees " << s[0].mu << endl;
            cout << "Total induced charge on the interface " << nanoParticle->total_induced_charge(s) << endl;
            cout << "Constraint is (zero if satisfied) " << constraint(s, ion, nanoParticle) << endl;
            cout << "Time derivative of the constraint is " << dotconstraint(s) << endl;
            cout << "Initial force on fake degree at vertex 0 " << s[0].fw << endl;
            cout << "Initial fake kinetic energy " << fake_ke << endl;
            cout << "Initial ion kinetic energy " << particle_ke << endl;
            cout << "Inital potential energy " << potential_energy << endl;
            cout << "Initial (real + fake) system energy " << fake_ke + particle_ke + potential_energy << endl;
            cout << "Chain length (L+1) implementation " << real_bath.size() << endl;
            cout << "Main thermostat temperature " << real_bath[0].T << endl;
            cout << "Main thermostat mass " << real_bath[0].Q << endl;
            cout << "Fake chain length (L+1) implementation " << fake_bath.size() << endl;
            cout << "Main fake thermostat temperature " << fake_bath[0].T << endl;
            cout << "Main fake thermostat mass " << fake_bath[0].Q << endl;
            nanoParticle->printBinSize();    //print the bin size
            cout << "Number of steps " << cpmdremote.steps << endl;
            cout << "Write basic files every " << cpmdremote.writedata << " steps" << endl;
            cout << "Production begins at " << cpmdremote.hiteqm << endl;
            cout << "Sampling frequency " << cpmdremote.freq << endl;
            cout << "Extra computation every " << cpmdremote.extra_compute << " steps" << endl;
            cout << "Verification every " << cpmdremote.verify << " steps" << endl;
            cout << "Write density profile every " << cpmdremote.writedensity << endl;
        }
        cout << "Time step " << cpmdremote.timestep << endl;
    }
    double energy_samples = 0;
    double average_functional_deviation = 0.0;        // average deviation from the B O surface
    double verification_samples = 0;            // number of samples used to verify C P M D evolution
    int moviestart = 0;                    // starting point of the movie
    int moviefreq = cpmdremote.freq * 10;                    // frequency of shooting the movie

    double density_profile_samples = 0;            // number of samples used to estimate density profile

    long double expfac_real, expfac_fake;            // exponential factors pre-computed, useful in velocity Verlet update routine

    double percentage = 0, percentagePre = -1;

    // Output the zeroth timestep in a movie:
    make_movie(0, ion, nanoParticle);

    // Part II : Propagate
    for (int num = 1; num <= cpmdremote.steps; num++) {

        // INTEGRATOR
        //! begins
        // reverse update of Nose-Hoover chain
        for (int j = real_bath.size() - 1; j > -1; j--)
            update_chain_xi(j, real_bath, cpmdremote.timestep,
                            particle_ke);            // update xi for real baths in reverse order
        for (unsigned int j = 0; j < real_bath.size(); j++)
            real_bath[j].update_eta(cpmdremote.timestep);                    // update eta for real baths

        expfac_real = exp(-0.5 * cpmdremote.timestep * real_bath[0].xi);
        // Modified velocity Verlet (with thermostat effects) for Real system
        for (unsigned int i = 0; i < ion.size(); i++)
            ion[i].new_update_velocity(cpmdremote.timestep, real_bath[0],
                                       expfac_real);    // update particle velocity half time step


        for (unsigned int i = 0; i < ion.size(); i++)
            ion[i].update_position(cpmdremote.timestep);                    // update particle position full time step

        if (nanoParticle->POLARIZED) {
            for (int j = fake_bath.size() - 1; j > -1; j--)
                update_chain_xi(j, fake_bath, cpmdremote.timestep,
                                fake_ke);            // update xi for fake baths in reverse order
            for (unsigned int j = 0; j < fake_bath.size(); j++)
                fake_bath[j].update_eta(cpmdremote.timestep);                    // update eta for fake baths
            // pre-compute expfac_real and expfac_fake to be used in velocity Verlet
            expfac_fake = exp(-0.5 * cpmdremote.timestep * fake_bath[0].xi);
            // Modified velocity Verlet (with thermostat effects) for Fake system
            for (unsigned int k = 0; k < s.size(); k++)
                s[k].new_update_velocity(cpmdremote.timestep, fake_bath[0],
                                         expfac_fake);        // update fake velocity half time step
            for (unsigned int k = 0; k < s.size(); k++)                        // update fake position full time step
                s[k].update_position(cpmdremote.timestep);

            SHAKE(s, ion, nanoParticle,
                  cpmdremote);                            // shake to ensure constraint is satisfied
        }

        //cout << "Pre num =" << num <<  ", pos: "<< ion[0].posvec << ", force "<<  ion[0].forvec << ", vel "<<  ion[0].velvec << endl;
        for_cpmd_calculate_force(s, ion, nanoParticle);                    // calculate forces on ion and fake degree
        //cout << "Post num =" << num <<  ", pos: "<< ion[0].posvec << ", force "<<  ion[0].forvec << ", vel "<<  ion[0].velvec << endl;

        for (unsigned int i = 0; i < ion.size(); i++)
            ion[i].new_update_velocity(cpmdremote.timestep, real_bath[0],
                                       expfac_real);    // update particle velocity half time step


        if (nanoParticle->POLARIZED) {
            for (unsigned int k = 0; k < s.size(); k++)
                s[k].new_update_velocity(cpmdremote.timestep, fake_bath[0],
                                         expfac_fake);        // update fake velocity half time step

            RATTLE(s);                                        // rattle to ensure time derivative of the constraint is satisfied

            // kinetic energies needed to set canonical ensemble
            // Forward Nose-Hoover chain

            fake_ke = fake_kinetic_energy(s);
            for (unsigned int j = 0; j < fake_bath.size(); j++)
                fake_bath[j].update_eta(cpmdremote.timestep);                    // update eta for fake baths
            for (unsigned int j = 0; j < fake_bath.size(); j++)
                update_chain_xi(j, fake_bath, cpmdremote.timestep,
                                fake_ke);            // update xi for fake baths in forward order
        }


        particle_ke = particle_kinetic_energy(ion);
        for (unsigned int j = 0; j < real_bath.size(); j++)
            real_bath[j].update_eta(cpmdremote.timestep);                    // update eta for real baths
        for (unsigned int j = 0; j < real_bath.size(); j++)
            update_chain_xi(j, real_bath, cpmdremote.timestep,
                            particle_ke);            // update xi for real baths in forward order
        //! ends

        // extra computations
        if (num % cpmdremote.extra_compute == 0) {
            energy_samples++;
            compute_n_write_useful_data(num, ion, s, real_bath, fake_bath, nanoParticle);
            // write basic files
            write_basic_files(cpmdremote.writedata, num, ion, s, real_bath, fake_bath, nanoParticle);
        }
        // verify with F M D
        if (nanoParticle->POLARIZED && num % cpmdremote.verify == 0) {
            double functional_deviation = verify_with_FMD(num, s, ion, nanoParticle, fmdremote, cpmdremote);
            verification_samples++;
            average_functional_deviation += functional_deviation;
        }



        // make a movie
        if (num >= moviestart && num % moviefreq == 0)
            make_movie(num, ion, nanoParticle);

        // compute density profile
        if (num >= cpmdremote.hiteqm && (num % cpmdremote.freq == 0)) {

            density_profile_samples++;
            nanoParticle->updateSamples(density_profile_samples);
            nanoParticle->updateStep(num);
            nanoParticle->compute_density_profile();
            energy_functional(s, ion, nanoParticle);  // Assess the PE (specifically ES component for Diehl's)
            nanoParticle->compute_effective_charge(num, condensedIonsPerStep, ion, nanoParticle, cpmdremote);
        }
        //percentage calculation
        if (world.rank() == 0)
        {
            //percentage calculation
            if(!cpmdremote.verbose)
                percentage=roundf(num/(double)cpmdremote.steps*100);
            else
                percentage=roundf(num/(double)cpmdremote.steps*100 * 10) / 10;
            //percentage output
            if(percentage!=percentagePre)
            {
                if(!cpmdremote.verbose)
                {
                    int progressBarVal=(int) (percentage+0.5);
                    printf("=PROGRESS=>%d\n",progressBarVal);
                }else
                {
                    double fraction_completed = percentage/100;
                    progressBar(fraction_completed);
                }
                percentagePre=percentage;

            }
        }

    }

    // Part III : Analysis
    // Final density profile
    nanoParticle->compute_final_density_profile();

    ofstream final_induced_density("outfiles/final_induced_density.dat");
    for (unsigned int k = 0; k < s.size(); k++)
        final_induced_density << k + 1 << setw(15) << s[k].theta << setw(15) << s[k].phi << setw(15) << s[k].w << endl;
    ofstream final_configuration("outfiles/final_configuration.dat");
    for (unsigned int i = 0; i < ion.size(); i++)
        final_configuration << ion[i].posvec << endl;
    if (world.rank() == 0 && cpmdremote.verbose) {
        cout << "Number of samples used to compute energy" << setw(10) << energy_samples << endl;
        cout << "Number of samples used to get density profile, effective charge" << setw(10) << density_profile_samples << endl;
        if (nanoParticle->POLARIZED)
            cout << "Number of samples used to verify on the fly results" << setw(10) << verification_samples << endl;
        if (nanoParticle->POLARIZED)
            cout << "Average deviation of the functional from the BO surface" << setw(15)
                 << average_functional_deviation / verification_samples << endl;
    }
    return;
}
// End of CPMD routine
