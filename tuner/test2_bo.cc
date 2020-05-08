#include "gp/gp.h"
#include "gp/rprop.h"
#include "gp/cg.h"
#include "gp/gp_utils.h"

#include <cmath>
#include <iomanip>
#include <iostream>

using namespace std;
using Matrix = Eigen::MatrixXd;
using Vector = Eigen::VectorXd;
using GP = libgp::GaussianProcess;

#define Pi 3.141592653

double fin(double X){
    return -sin(3*X) - X*X + 0.7*X;
}
Vector finM(Matrix X){
    int r = X.rows();
    Vector V(r);
    int c = X.cols();
    double C = 0;
    for (int i = 0; i < r; i++){
        for (int j = 0; j < c; j++){
            C += fin(X(i,j)) ;
        }
        V[i]=C;
        C=0;
    }
    return V;
}
Vector square(Matrix X){
    int r = X.rows();
    Vector V(r);
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

GP* init( int input_dim ) {
    GP *gp = new GP(input_dim, "CovSum ( CovSEiso, CovNoise)");
    Vector params(gp->covf().get_param_dim());
    params << 0, 0, -2;
    gp->covf().set_loghyper(params);
    int N = 10;
    Matrix X(N, input_dim);
    X.setRandom();
    X = X*2;
    //Vector y = gp->covf().draw_random_sample(X);
    Vector y = finM(X);
    for(size_t i = 0; i < N; ++i) {
        double x[input_dim];
        for(int j = 0; j < input_dim; ++j) {
            x[j] = X(i,j);
            //cout<<"x,y = \t"<<x[j]<<"\t"<<y(i)<<endl;
        }
        gp->add_pattern(x, y(i));
    }
    return gp;
}

void validate(GP *gp, double x1, double x2){
  double x[2] = {x1,x2};
  cout<<"f("<<x1<<")\t="<<gp->f(x)<<"\t""var="<<gp->var(x)<<endl;
}
void validate_all(GP *gp){
  cout<<"-------------------------------"<<endl;
  validate(gp,  -1,0);
  validate(gp,-0.5,0);
  validate(gp,   0,0);
  validate(gp, 0.5,0);
  validate(gp,   1,0);
  validate(gp, 1.5,0);
  validate(gp,   2,0);
  cout<<"-------------------------------"<<endl;
}

void fit1(GP *gp){
  libgp::RProp rprop;
  rprop.init();
  int ver = 0;
  int prt = 0;
  rprop.maximize(gp, 50, ver, prt);
}
///////////////////////////////////////////
//finM
Vector predict(GP *gp, Matrix X, bool var){
    Vector y(X.rows());
    for(int i = 0; i < X.rows(); i++) {
        double x[X.cols()];
        for(int j = 0; j < X.cols(); j++) {
            x[j] = X(i,j);
        }
        if(var) y[i] = gp->var(x);
        else    y[i] = gp->f(x);
    }
    return y;
}
double best_vec(Vector v, bool top_best){
    double max = v[0];
    for(int j = 1; j < v.size(); j++) {
        if(top_best && max>v[j])  max = v[j];
        else if(!top_best && max<v[j]) max = v[j];
    }
    return max;
}
double expectedImprovement(GP *gp, Vector x, double target) {
    double g = (gp->f(x.data()) - target) / sqrt(gp->var(x.data()));
    double ei = sqrt(gp->var(x.data())) * (g * libgp::Utils::cdf_norm(g)   +   1.0/(2*M_PI) * exp(-0.5*g*g)   );
    return ei;
}
double probabilityImprovement(libgp::GaussianProcess *gp, Eigen::VectorXd x, double target) {
    return libgp::Utils::cdf_norm( (gp->f(x.data()) - target - 0.01) / gp->var(x.data()));
}
void expected_improvement(GP *gp, Matrix X, Matrix Xsample, Vector Ysample, double buf=0.01){
    bool top_best = true;
    Vector mu = predict( gp, X, false);
    Vector sigma = predict( gp, X, true);
    Vector mu_sample = predict(gp,Xsample,false);
    double mu_sample_best = best_vec(mu_sample,top_best);
    //double scaling_factor = -1 ** top_best;
    //Vector I = mu - mu_sample_best - buf;
    //Vector Z = I / sigma;
    //double ei = I * normalCDF(Z) + sigma * normalPDF(Z);
}

int main(){
    int input_dim=1;
    GP *gp = init(input_dim);

    cout<<"before training"<<endl;
    validate_all(gp);
    fit1(gp);
    cout<<"after training"<<endl;
    validate_all(gp);
}


////////////

double normalCDF(double x){ // Phi(-∞, x) aka N(x)
    return erfc(-x / sqrt(2))/2;
}
double normalPDF(double x){
    return exp(-x*x / 2.0) / sqrt(2.0 * Pi);
}
double normalPDF(const Vector& mean, const Matrix& covar, const Vector& input) {
  // -0.5 * (x-mu)^T * Sigma^-1 * (x-mu)
  double inside_exp = -0.5*(mean-input).dot(covar.inverse()*(mean-input));
  // (1/sqrt( (2*PI)^k * |Sigma| )) * exp(-0.5 * (x-mu)^T * Sigma^-1 * (x-mu))
  return pow(pow(2*M_PI,input.size()*covar.determinant()),-0.5)*exp(inside_exp);
}
