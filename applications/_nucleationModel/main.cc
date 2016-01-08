//Coupled Cahn-Hilliard and Allen-Cahn implementation with nucleation
//general headers
#include "../../include/dealIIheaders.h"
//coupled Cahn-Hilliard and Allen-Cahn problem headers
#include "parameters.h"
#include "../../src/models/diffusion/coupledCHAC.h"
#include <time.h>


//structure representing each nucleus 
struct nucleus{
  unsigned int index;
  dealii::Point<problemDIM> center;
  double radius;
  double seededTime, seedingTime;
};
//vector of all nucleus seeded in the problem
std::vector<nucleus> nuclei, localNuclei;

//initial condition function for concentration
template <int dim>
class InitialConditionC : public Function<dim>
{
public:
  InitialConditionC () : Function<dim>(1) {
    //std::srand(Utilities::MPI::this_mpi_process(MPI_COMM_WORLD)+1);
    std::srand(time(NULL));
  }
  double value (const Point<dim> &p, const unsigned int component = 0) const
  {
    //return the value of the initial concentration field at point p 
    return 0.03 + 1.0e-3*(2*(0.5 - (double)(std::rand() % 100 )/100.0));
  }
};

//apply initial conditions
template <int dim>
void CoupledCHACProblem<dim>::applyInitialConditions()
{
  unsigned int fieldIndex;
  
  //call initial condition function for c
  fieldIndex=this->getFieldIndex("c");
  VectorTools::interpolate (*this->dofHandlersSet[fieldIndex],		\
			    InitialConditionC<dim>(),			\
			    *this->solutionSet[fieldIndex]);
  //call initial condition function for n
  fieldIndex=this->getFieldIndex("n");
  VectorTools::interpolate (*this->dofHandlersSet[fieldIndex],		\
			    ZeroFunction<dim>(1),			\
			    *this->solutionSet[fieldIndex]);
}

//nucleation model implementation
template <int dim>
void CoupledCHACProblem<dim>::modifySolutionFields()
{
  //current time
  double t=this->currentTime;
  unsigned int inc=this->currentIncrement;
  double dx=spanX/std::pow(2.0,refineFactor);
  double rand_val;
  int count = 0;
  //nucleation parameters
  double minDistBetwenNuclei=1.0*spanX/10.0;
  double nRadius = spanX/50.0;
  unsigned int maxNumberNuclei=5;
  //unsigned int rand_chance = .0000005;
  unsigned int rand_scale = 10000;

  //get the list of node points in the domain
  std::map<dealii::types::global_dof_index, dealii::Point<dim> > support_points;
  dealii::DoFTools::map_dofs_to_support_points (dealii::MappingQ1<dim>(), *this->dofHandlersSet[0], support_points);   
  //fields
  vectorType* n=this->solutionSet[this->getFieldIndex("n")];
  vectorType* c=this->solutionSet[this->getFieldIndex("c")];
  const double k1 = 0.0001;
  const double k2 = 1.0;
  const double c0 = 0.300;
  double J = 0.0;
  //delete the previous entries in the nuclei vector (old nucleus are still retained in the localNuclei vector)
  nuclei.clear();
  
  //populate localNuclei vector
  if (inc <= timeIncrements-skipOutputSteps){
    nucleus* temp;
    //add nuclei based on concentration field values
    //loop over all points in the domain    
    for (typename std::map<dealii::types::global_dof_index, dealii::Point<dim> >::iterator it=support_points.begin(); it!=support_points.end(); ++it){
      unsigned int dof=it->first;
      //set only local owned values of the parallel vector
      if (n->locally_owned_elements().is_element(dof)){
	dealii::Point<dim> nodePoint=it->second;
	double nValue=(*n)(dof);
	double cValue=(*c)(dof);
	
	rand_val = (rand() % rand_scale)/((double)rand_scale);
	//std::cout << rand_val<< '\n';
	//if (cValue>0.0305) {
	//and (localNuclei.size()<maxNumberNuclei)){
	J = k1*exp(-k2/(cValue-c0));
	//std::cout << J << " " << rand_val << "\n";
	
	if (rand_val <= J){
	  //loop over all existing nuclei to check if they are in the vicinity
	  bool isClose=false;
	  for (std::vector<nucleus>::iterator thisNuclei=localNuclei.begin(); thisNuclei!=localNuclei.end(); ++thisNuclei){
	    if (thisNuclei->center.distance(nodePoint)<minDistBetwenNuclei){
	      isClose=true;
	    }
	  }
	  if (!isClose){
	    temp = new nucleus;
	    temp->index=localNuclei.size(); 
	    temp->center=nodePoint;
	    temp->radius=spanX/50.0;
	    //temp->seededTime=t;
	    //temp->seededTime=this->currentTime;
	    temp->seededTime = 0.0;
	    //temp->seedingTime=this->finalTime;
	    temp->seedingTime = t + 30.0*timeStep;
	    localNuclei.push_back(*temp);
	  }
	}
      }
    }

    //filter nuclei by comparing with other processors
    int numProcs=Utilities::MPI::n_mpi_processes(MPI_COMM_WORLD);        
    int thisProc=Utilities::MPI::this_mpi_process(MPI_COMM_WORLD);
    std::vector<int> numNucleiInProcs(numProcs, 0);
    //send nuclei information to processor 0
    int numNuclei=localNuclei.size();
    //send information about number of nuclei to processor 0
    if (thisProc!=0){
      MPI_Send(&numNuclei, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
    }
    else{
      numNucleiInProcs[0]=numNuclei;
      for (int proc=1; proc<numProcs; proc++){
	MPI_Recv(&numNucleiInProcs[proc], 1, MPI_INT, proc, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    //filter nuclei in processor zero
    //receive nuclei info from all processors
    if (thisProc!=0){
      if (numNuclei>0){
	std::vector<double> tempData((dim+3)*numNuclei);
	unsigned int i=0;
	for (std::vector<nucleus>::iterator thisNuclei=localNuclei.begin(); thisNuclei!=localNuclei.end(); ++thisNuclei){
	  tempData[i*(dim+3)]=thisNuclei->radius;
	  tempData[i*(dim+3)+1]=thisNuclei->seededTime;
	  tempData[i*(dim+3)+2]=thisNuclei->seedingTime;
	  for (unsigned int j=0; j<dim; j++) tempData[i*(dim+3)+3+j]=thisNuclei->center[j];
	  i++;
	}
	MPI_Send(&tempData[0], numNuclei*(dim+3), MPI_DOUBLE, 0, 1, MPI_COMM_WORLD);
      }
    }
    else{
      //temporary array to store all the nuclei
      std::vector<std::vector<double>*> tempNuceli(numProcs);
      for (int proc=0; proc<numProcs; proc++) {
	std::vector<double>* temp=new std::vector<double>(numNucleiInProcs[proc]*(dim+3));
	if (numNucleiInProcs[proc]>0){
	  if (proc==0){
	    unsigned int i=0;
	    for (std::vector<nucleus>::iterator thisNuclei=localNuclei.begin(); thisNuclei!=localNuclei.end(); ++thisNuclei){
	      (*temp)[i*(dim+3)]=thisNuclei->radius;
	      (*temp)[i*(dim+3)+1]=thisNuclei->seededTime;
	      (*temp)[i*(dim+3)+2]=thisNuclei->seedingTime;
	      for (unsigned int j=0; j<dim; j++) (*temp)[i*(dim+3)+3+j]=thisNuclei->center[j];
	      i++;
	    }    
	  }
	  else{
	    MPI_Recv(&((*temp)[0]), numNucleiInProcs[proc]*(dim+3), MPI_DOUBLE, proc, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);	
	  }
	  tempNuceli[proc]=temp;
	}
      }
    
      //filter the nuclei and add to nuclei vector in processor zero
      for (int proc1=0; proc1<numProcs; proc1++) {
	for (int i1=0; i1<numNucleiInProcs[proc1]; i1++){
	  double rad1=(*tempNuceli[proc1])[i1*(dim+3)];
	  double time1=(*tempNuceli[proc1])[i1*(dim+3)+1];
	  double seedingTime1=(*tempNuceli[proc1])[i1*(dim+3)+2];
	  dealii::Point<dim> center1;
	  for (unsigned int j1=0; j1<dim; j1++) {
	    center1[j1]=(*tempNuceli[proc1])[i1*(dim+3)+3+j1];
	  }
	  bool addNuclei=true;
	  //check if this nuceli present in any other processor
	  for (int proc2=0; proc2<numProcs; proc2++) {
	    if (proc1!=proc2){
	      for (int i2=0; i2<numNucleiInProcs[proc2]; i2++){
		double rad2=(*tempNuceli[proc2])[i2*(dim+3)];
		double time2=(*tempNuceli[proc2])[i2*(dim+3)+1];
		dealii::Point<dim> center2;
		for (unsigned int j2=0; j2<dim; j2++) center2(j2)=(*tempNuceli[proc2])[i2*(dim+3)+3+j2];
		if ((center1.distance(center2)<=minDistBetwenNuclei) && (time1>=time2)){
		  addNuclei=false;
		  break;
		}
	      }
	      if (!addNuclei) {break;}
	    }
	  }
	  if (addNuclei){
	    temp = new nucleus;
	    temp->index=nuclei.size();
	    temp->radius=rad1;
	    temp->seededTime=time1;
	    temp->seedingTime=seedingTime1;
	    temp->center=center1;
	    nuclei.push_back(*temp);
	  }
	}
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    //disperse nuclei to all other processors
    int numGlobalNuclei;
    if (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD)==0) {numGlobalNuclei=nuclei.size();}
    MPI_Bcast(&numGlobalNuclei, 1, MPI_INT, 0, MPI_COMM_WORLD);
    this->pcout << "total number of nuclei currently seeded : "  << numGlobalNuclei << std::endl;
    MPI_Barrier(MPI_COMM_WORLD);
    //
    std::vector<double> temp2(numGlobalNuclei*(dim+3));
    if (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD)==0){
      unsigned int i=0;
      for (std::vector<nucleus>::iterator thisNuclei=nuclei.begin(); thisNuclei!=nuclei.end(); ++thisNuclei){
	temp2[i*(dim+3)]=thisNuclei->radius;
	temp2[i*(dim+3)+1]=thisNuclei->seededTime;
	temp2[i*(dim+3)+2]=thisNuclei->seedingTime;
	for (unsigned int j=0; j<dim; j++) temp2[i*(dim+3)+3+j]=thisNuclei->center[j];
	i++;
      }
    }
    MPI_Bcast(&temp2[0], numGlobalNuclei*(dim+3), MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);
    //receive all nuclei
    if (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD)!=0){
      for(unsigned int i=0; i<numGlobalNuclei; i++){
	temp = new nucleus;
	temp->index=nuclei.size();
	temp->radius=temp2[i*(dim+3)];
	temp->seededTime=temp2[i*(dim+3)+1];
	temp->seedingTime=temp2[i*(dim+3)+2];
	dealii::Point<dim> tempCenter;
	for (unsigned int j=0; j<dim; j++) tempCenter[j]=temp2[i*(dim+3)+3+j];
	temp->center=tempCenter;
	nuclei.push_back(*temp);
      }
    }
  }

  //seed nuclei
  unsigned int fieldIndex=this->getFieldIndex("n");
  for (std::vector<nucleus>::iterator thisNuclei=nuclei.begin(); thisNuclei!=nuclei.end(); ++thisNuclei){
    //
    dealii::Point<dim> center=thisNuclei->center;
    double radius=thisNuclei->radius;
    double seededTime=thisNuclei->seededTime;
    double seedingTime=thisNuclei->seedingTime;
    //this->pcout << "times: " << t << " " << seededTime << " " << seedingTime << std::endl;
    //loop over all points in the domain
    for (typename std::map<dealii::types::global_dof_index, dealii::Point<dim> >::iterator it=support_points.begin(); it!=support_points.end(); ++it){
      unsigned int dof=it->first;
      //set only local owned values of the parallel vector
      if (n->locally_owned_elements().is_element(dof)){
	dealii::Point<dim> nodePoint=it->second;
	//check conditions and seed nuclei
	double r=nodePoint.distance(center);
	if (r<=(radius+3*dx)){
	  if ((t>seededTime) && (t<(seededTime+seedingTime))){
	    //this->pcout << "times: " << t << " " << seededTime << " " << seedingTime << std::endl;
	    (*n)(dof)=0.5*(1.0-std::tanh((r-spanX/50.0)/(dx)));
	  }
	}
      }
    }
  }
}

//main
int main (int argc, char **argv)
{
  Utilities::System::MPI_InitFinalize mpi_initialization(argc, argv,numbers::invalid_unsigned_int);
  try
    {
      deallog.depth_console(0);
      CoupledCHACProblem<problemDIM> problem;
      problem.fields.push_back(Field<problemDIM>(SCALAR, PARABOLIC, "n"));
      problem.fields.push_back(Field<problemDIM>(SCALAR, PARABOLIC, "c"));
      problem.init(); 
      problem.solve();
    }
  catch (std::exception &exc)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Exception on processing: " << std::endl
                << exc.what() << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }
  catch (...)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Unknown exception!" << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }
  
  return 0;
}