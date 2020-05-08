#include "gp/gp.h"
#include "gp/rprop.h"
#include "gp/cg.h"
#include "gp/gp_utils.h"

#include <cmath>
#include <iostream>

using namespace std;

Eigen::VectorXd square(Eigen::MatrixXd X){
    int r = X.rows();
    Eigen::VectorXd V(r);
    int c = X.cols();
    auto C = X(0,0);
    C=0;
    for (int i = 0; i < r; ++i){
        for (int j = 0; j < c; ++j){
            C += X(i,j)*X(i,j);
        }
        V[i]=C;
        C=0;
    }
    return V;
}

libgp::GaussianProcess* init( int input_dim ) {
    libgp::GaussianProcess* gp = new libgp::GaussianProcess(input_dim, "CovSum ( CovSEiso, CovNoise)");
    Eigen::VectorXd params(gp->covf().get_param_dim());
    params << 0, 0, -2;
    gp->covf().set_loghyper(params);
    int N = 50;
    Eigen::MatrixXd X(N, input_dim);
    X.setRandom();
    X = X*10;
    //Eigen::VectorXd y = gp->covf().draw_random_sample(X);
    Eigen::VectorXd y = - square(X);
    for(size_t i = 0; i < N; ++i) {
        double x[input_dim];
        //printf("x[] =  ");
        for(int j = 0; j < input_dim; ++j) {
            x[j] = X(i,j);
            //cout<<"x,y = \t"<<x[j]<<"\t"<<y(i)<<endl;
            //printf("%02f \t",x[j] );
        }
        //cout<<"--->    "<<y(i)<<endl;
        gp->add_pattern(x, y(i));
    }
    return gp;
}

void validate(libgp::GaussianProcess* gp, double x1, double x2){
  double x[2] = {x1,x2};
  cout<<"f="<<gp->f(x)<<"\t""var="<<gp->var(x)<<endl;
}

void test(libgp::GaussianProcess* gp ){
  cout<<"before training"<<endl;
  validate(gp,0,0);
  validate(gp,-2,0);
  validate(gp,2,0);

  libgp::RProp rprop;
  rprop.init();
  int ver = 0;
  int prt = 0;
  rprop.maximize(gp, 50, ver, prt);

  //Eigen::VectorXd p = gp->covf().get_loghyper();
  //cout<<"params=\n"<<p<<endl;
  //double ll = gp->log_likelihood();
  //cout<<"log_likelihood="<<ll<<endl;

  cout<<"after training"<<endl;
  validate(gp,0,0);
  validate(gp,-2,0);
  validate(gp,2,0);
}

int main(){
    int input_dim=1;
    libgp::GaussianProcess * gp = init(input_dim );
    test(gp);
}
