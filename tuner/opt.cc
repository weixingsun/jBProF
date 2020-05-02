#include "gp/gp.h"
#include "gp/rprop.h"
#include "gp/cg.h"
#include "gp/gp_utils.h"

#include <cmath>
#include <iostream>

using namespace std;

int input_dim, param_dim;
libgp::GaussianProcess * gp;
size_t n;

void init() {
    input_dim = 3, param_dim = 3;
    gp = new libgp::GaussianProcess(input_dim, "CovSum ( CovSEiso, CovNoise)");
    Eigen::VectorXd params(param_dim);
    params << 0, 0, log(0.01);
    gp->covf().set_loghyper(params);
    n = 50;
    Eigen::MatrixXd X(n, input_dim);
    X.setRandom();
    X = X*10;
    Eigen::VectorXd y = gp->covf().draw_random_sample(X);
    for(size_t i = 0; i < n; ++i) {
        double x[input_dim];
        for(int j = 0; j < input_dim; ++j) {
            x[j] = X(i,j);
            cout<<"x,y = "<<x[j]<<","<<y(i)<<endl;
        }
        gp->add_pattern(x, y(i));
    }
}


void test(){
  Eigen::VectorXd params(param_dim);
  params << -1, -1, -1;
  gp->covf().set_loghyper(params);

  libgp::RProp rprop;
  rprop.init();
  rprop.maximize(gp, 50, 0);

  double c1 = gp->covf().get_loghyper()(0);
  double c2 = gp->covf().get_loghyper()(1);
  cout<<"c1="<<c1<<endl;
  cout<<"c2="<<c2<<endl;

  //f = gp.f(x);
  //v = gp.var(x);

}

int main(){
    init();
    test();
}
