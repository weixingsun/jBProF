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

libgp::GaussianProcess* init( int input_dim, int param_dim) {
    libgp::GaussianProcess* gp = new libgp::GaussianProcess(input_dim, "CovSum ( CovSEiso, CovNoise)");
    Eigen::VectorXd params(param_dim);
    params << 0, 0, log(0.01);                                            ////////////////////////////////////////////////////////////////////
        Eigen::VectorXd p0 = gp->covf().get_loghyper();
        cout<<"p0.size()="<<p0.size()<<"\n"<<p0<<endl;
        cout<<"params.size()="<<params.size()<<"\n"<<params<<endl;
    gp->covf().set_loghyper(params);
    int N = 50;
    Eigen::MatrixXd X(N, input_dim);
    X.setRandom();
    X = X*10;
    //Eigen::VectorXd y = gp->covf().draw_random_sample(X);
    Eigen::VectorXd y = -1*square(X);
    for(size_t i = 0; i < N; ++i) {
        double x[input_dim];
        printf("x[] =  ");
        for(int j = 0; j < input_dim; ++j) {
            x[j] = X(i,j);
            //cout<<"x,y = \t"<<x[j]<<"\t"<<y(i)<<endl;
            printf("%02f \t",x[j] );
        }
        cout<<"--->    "<<y(i)<<endl;
        gp->add_pattern(x, y(i));
    }
    return gp;
}


void test(libgp::GaussianProcess* gp,int param_dim){
  Eigen::VectorXd params(param_dim);
  params <<  -1, -1, -1;                                                  ////////////////////////////////////////////////////////////////////
  Eigen::VectorXd p0 = gp->covf().get_loghyper();
  cout<<"p0=\n"<<p0<<endl;
  cout<<"params=\n"<<params<<endl;
  gp->covf().set_loghyper(params);

  libgp::RProp rprop;
  rprop.init();
  int ver = 1;
  rprop.maximize(gp, 50, ver);

  Eigen::VectorXd p = gp->covf().get_loghyper();
  cout<<"p1=\n"<<p<<endl;

  //f = gp.f(x);
  //v = gp.var(x);

}

int main(){
    int input_dim=1, param_dim=3;
    libgp::GaussianProcess * gp = init(input_dim, param_dim);
    test(gp,param_dim);
}
