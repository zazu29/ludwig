########################################################################
#                                                                      #
#  extract_sp.py                                                          #
#                                                                      #
#  Script for batch processing of data files in vtk-format             #
#  for visualisation in Paraview.                                      #
#                                                                      #
#  Requires executable 'extract' with corresponding flags set          #
#
#  Usage: $> python extract.py                                         #
#                                                                      #
#  Edinburgh Soft Matter and Statistical Physics Group                 #
#  Edinburgh Parallel Computing Centre                                 #
#  University of Strathclyde, Glasgow, UK                              #
#                                                                      #
#  Contributing authors:                                               #
#  Kevin Stratford (kevin@epcc.ed.ac.uk)                               #
#  Oliver Henrich  (oliver.henrich@strath.ac.uk)                       #
#                                                                      #
#
#  (c) 2011-2018 The University of Edinburgh                           #
#                                                                      #
########################################################################

import sys, os

nstart=int(sys.argv[1])# Start timestep
nint=int(sys.argv[2])	# Increment
nend=int(sys.argv[3])	# End timestep
pn=sys.argv[4] #processor number
ngroup=1	# Number of output groups

a0_squ=3.0      # the radius of squirmer
a0_poly=0.179   # the radius of monomer 

vel=0		# Switch for velocity 
q=0		# Switch for Q-tensor
phi=0		# Switch for binary fluid
psi=0		# Switch for electrokinetics
fed=0		# Switch for free energy
colcds=0	# Switch for colloid coordinate
colcdsvel=0	# Switch for colloid coordinate and lattice velocity
squ_poly_cds=1	# Switch for squirmer and polymer coordinate
squ_poly_cdsvel=0 # Switch for squirmer and polymer coordinate and lattice velocity

# Set lists for analysis
metafile=[]
filelist=[]

if vel==1:
	metafile.append('vel.00%d-001.meta' % ngroup)
	filelist.append(pn+'filelist_vel')
	os.system('rm '+pn+'filelist_vel')
	for i in range(nstart,nend+nint,nint):
		os.system('ls -t1 vel-%08.0d.00%d-001 >> %dfilelist_vel' % (i,ngroup,int(pn)))

if q==1:
	metafile.append('q.00%d-001.meta' % ngroup)
	filelist.append(pn+'filelist_q')
	os.system('rm '+pn+'filelist_q')
	for i in range(nstart,nend+nint,nint):
		os.system('ls -t1 q-%08.0d.00%d-001 >> %dfilelist_q' % (i,ngroup,int(pn)))

if phi==1:
	metafile.append('phi.%03.0d-001.meta' % ngroup)
	filelist.append(pn+'filelist_phi')
	os.system('rm '+pn+'filelist_phi')
	for i in range(nstart,nend+nint,nint):
		os.system('ls -t1 phi-%08.0d.%03.0d-001 >> %dfilelist_phi' % (i,ngroup,int(pn)))

if psi==1:
        metafile.append('psi.%03.0d-001.meta' % ngroup)
        filelist.append(pn+'filelist_psi')
        os.system('rm '+pn+'filelist_psi')
        for i in range(nstart,nend+nint,nint):
                os.system('ls -t1 psi-%08.0d.%03.0d-001 >> %dfilelist_psi' % (i,ngroup,int(pn)))

if fed==1:
        metafile.append('fed.%03.0d-001.meta' % ngroup)
        filelist.append(pn+'filelist_fed')
        os.system('rm '+pn+'filelist_fed')
        for i in range(nstart,nend+nint,nint):
                os.system('ls -t1 fed-%08.0d.%03.0d-001 >> %dfilelist_fed' % (i,ngroup,int(pn)))

os.system('gcc -o vtk_extract vtk_extract.c -lm')

if (colcds==1) or (colcdsvel==1):
	metafile.append('')
	filelist.append(pn+'filelist_colloid')
	os.system('rm '+pn+'filelist_colloid')
	for i in range(nstart,nend+nint,nint):
		os.system('ls -t1 config.cds%08.0d.001-001 >> %dfilelist_colloid' % (i,int(pn)))

if (squ_poly_cds==1) or (squ_poly_cdsvel==1):
	metafile.append('')
	filelist.append(pn+'filelist_squ_poly')
	os.system('rm '+pn+'filelist_squ_poly')
	for i in range(nstart,nend+nint,nint):
		os.system('ls -t1 config.cds%08.0d.001-001 >> %dfilelist_squ_poly' % (i,int(pn)))

# Create vtk-files
for i in range(len(filelist)):
	if filelist[i] == pn+'filelist_vel' or filelist[i] == pn+'filelist_phi':
		datafiles=open(filelist[i],'r') 

		while 1:
			line=datafiles.readline()
			if not line: break

			print(('\n# Processing %s' % line)) 

			stub=line.split('.',1)
			os.system('./extract -a -k %s %s' % (metafile[i],stub[0]))

		datafiles.close

	if filelist[i] == pn+'filelist_q':
		datafiles=open(filelist[i],'r') 

		while 1:
			line=datafiles.readline()
			if not line: break

			print(('\n# Processing %s' % line)) 

			stub=line.split('.',1)
			os.system('./extract -a -k -s -d %s %s' % (metafile[i],stub[0]))

		datafiles.close

	if filelist[i] == pn+'filelist_colloid':
		datafiles=open(filelist[i],'r') 

		while 1:
			line=datafiles.readline()
			if not line: break

			print(('\n# Processing %s' % line))

			stub=line.split('.',2)
			datafilename = ('%s.%s' % (stub[0], stub[1]))
			outputfilename1 = ('col-%s.csv' % stub[1])
			outputfilename2 = ('velcol-%s.vtk' % stub[1])

			if colcds==1:
				os.system('./extract_colloids %s %d %s' % (datafilename,ngroup,outputfilename1))
			if colcdsvel==1:
				os.system('./extract_colloids %s %d %s %s' % (datafilename,ngroup,outputfilename1,outputfilename2))	

	if filelist[i] == pn+'filelist_squ_poly':
		datafiles=open(filelist[i],'r') 

		while 1:
			line=datafiles.readline()
			if not line: break

			print(('\n# Processing %s' % line))

			stub=line.split('.',2)
			datafilename = ('%s.%s' % (stub[0], stub[1]))
			outputfilename1 = ('squ-%s.csv' % stub[1])
			outputfilename2 = ('poly-%s.csv' % stub[1])
			outputfilename3 = ('velsqu-%s.vtk' % stub[1])

			if squ_poly_cds==1:
				os.system('./extract_squirmer_polymer %s %d %s %s %f %f' % (datafilename,ngroup,outputfilename1,outputfilename2,a0_squ,a0_poly))
			if squ_poly_cdsvel==1:
				os.system('./extract_squirmer_polymer %s %d %s %s %f %f %s' % (datafilename,ngroup,outputfilename1,outputfilename2,a0_squ,a0_poly,outputfilename3))	

os.system('rm '+pn+'filelist*')

print('# Done')
